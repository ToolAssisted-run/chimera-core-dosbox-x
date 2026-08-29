// See dosbox-driver.h. This is the author's BizHawk bizhawk.cpp carried over:
// the mechanisms (coroutine slicing, virtual clock, memory-file HDD, per-frame
// input injection) are unchanged; the ECL_EXPORT surface became the dosdrv_*
// API so the native reference build and the guest share every line.
#include "dosbox-driver.h"

#include <libco.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include <jaffarCommon/file.hpp>

#include "dosbox_conf_assets.h" // generated: conf presets + formatted disk heads
#include <zstd.h>

// DOSBox-X internals (include paths per sources.mk)
#include <config.h>
#include <sdlmain.h>
#include <render.h>
#include <keyboard.h>
#include <mixer.h>
#include <joystick.h>
#include <mouse.h>
#include <vga.h>
#include <mem.h>

#define DOS_DRIVE_A 0
#define DOS_DRIVE_D 3

// DOSBox entry points
extern int _main(int argc, char *argv[]);
extern void swapInDrive(int drive, unsigned int position);
static void runMain() { _main(0, nullptr); }

// ---- the coroutines: how the driver jumps in and out of dosbox ------------
cothread_t _emuCoroutine;
cothread_t _driverCoroutine;

// ---- the virtual clock ----------------------------------------------------
static double ticksTarget;
uint32_t _ticksElapsed;
uint32_t _GetTicks() { return _ticksElapsed; }
void _Delay(uint32_t ticks)
{
	_ticksElapsed += ticks;
	co_switch(_driverCoroutine);
}

// The DOS video mode's own refresh rate (written by vga_draw)
int _refreshRateNumerator = 0;
int _refreshRateDenominator = 0;

// ---- the in-guest file directory (the writable HDD lives here) ------------
jaffarCommon::file::MemoryFileDirectory _memFileDirectory;

// ---- audio: the mixer tees its converted output here ----------------------
std::vector<int16_t> _audioSamples;

// ---- keyboard: per-frame press/release sets (keyboard.cpp applies them) ---
static std::set<KBD_KEYS> _prevPressedKeys;
extern std::set<KBD_KEYS> _pressedKeys;
extern std::set<KBD_KEYS> _releasedKeys;

// ---- mouse ----------------------------------------------------------------
extern int mickey_threshold;
extern bool user_cursor_locked;
#define MOUSE_MAX_X 800
#define MOUSE_MAX_Y 600

// ---- drive activity (fat/iso drives set this; the drive light reads it) ---
bool _driveUsed = false;

// ---- configuration composition ---------------------------------------------

static std::string _composedConf;

// setup.cpp's ParseConfigFile reads this when it cannot open the file
extern "C" const char *chimera_composed_conf()
{
	return _composedConf.empty() ? nullptr : _composedConf.c_str();
}

std::string dosdrv_compose_conf(const DosDrvMachine &m)
{
	std::string conf((const char *)dosdrv_conf_base, dosdrv_conf_base_len);
	conf += "\n";
	for (size_t i = 0; i < sizeof dosdrv_conf_presets / sizeof dosdrv_conf_presets[0]; i++) {
		if (m.machinePreset == dosdrv_conf_presets[i].name) {
			conf.append((const char *)dosdrv_conf_presets[i].data, dosdrv_conf_presets[i].len);
			conf += "\n";
			break;
		}
	}
	// The section order below mirrors the author's BizHawk integration
	// (DOSBox.cs's configuration composition), so the same settings produce
	// the same machine and a finished BizHawk movie stays convertible.
	conf += "[joystick]\njoysticktype = ";
	conf += (m.joystick1 || m.joystick2) ? "2axis\n" : "none\n";
	conf += "[speaker]\n";
	if (m.pcSpeaker == "disabled") conf += "pcspeaker = Disabled\n";
	if (m.pcSpeaker == "enabled") conf += "pcspeaker = Enabled\n";
	conf += "\n[sblaster]\n";
	if (m.soundBlasterModel != "auto") conf += "sbtype = " + m.soundBlasterModel + "\n";
	if (m.soundBlasterIRQ != -1) conf += "irq = " + std::to_string(m.soundBlasterIRQ) + "\n";
	conf += "\n[dosbox]\n";
	if (m.memsizeMB >= 0) conf += "memsize = " + std::to_string(m.memsizeMB) + "\n";

	conf += "\n[autoexec]\n@echo off\n";
	// what the loaded file IS: the frontend mounts it under the fixed name
	// "rom" and the extension (rom.name) says how the machine takes it
	// extra images (the rom2..romN convention) join the mount as a swap
	// list; the disk-swap input controls cycle through them
	if (!m.floppyImages.empty() || !m.cdImages.empty()) {
		// project mode: the slot map named every image; each list mounts on
		// its own drive (mixed media works), listed order = swap order
		auto mountList = [&conf](char drive, const std::vector<std::string> &names, const char *type) {
			if (names.empty()) return;
			conf += std::string("imgmount ") + drive;
			for (const std::string &n : names) {
				bool quote = n.find(' ') != std::string::npos;
				conf += quote ? " \"" + n + "\"" : " " + n;
			}
			conf += std::string(" -t ") + type + "\n";
		};
		mountList('a', m.floppyImages, "floppy");
		mountList('d', m.cdImages, "iso");
	} else {
		std::string extras;
		for (int32_t i = 0; i < m.extraImageCount; i++) {
			extras += " rom" + std::to_string(i + 2);
		}
		static const char *floppyExts[] = { ".ima", ".img", ".xdf", ".fdi", ".hdm", ".nfd", ".d88" };
		for (const char *e : floppyExts) {
			if (m.romExt == e) { conf += "imgmount a rom" + extras + " -t floppy\n"; break; }
		}
		if (m.romExt == ".iso" || m.romExt == ".cue") {
			conf += "imgmount d rom" + extras + " -t iso\n";
		}
	}
	if (m.hddMounted) conf += "imgmount c HardDiskDrive.img\n";
	if (m.bootDrive == "a" || m.bootDrive == "c") {
		// the very last autoexec line: boot never returns to the shell
		// (a chimera addition; BizHawk movies run with bootDrive none)
		conf += "boot " + m.bootDrive + ":\n";
	}

	// BizHawk emits [cpu] and the machine override AFTER the autoexec
	conf += "\n[cpu]\n";
	if (m.cpuCycles >= 0) conf += "cycles = " + std::to_string(m.cpuCycles) + "\n";
	if (m.cpuType != "auto") conf += "cputype = " + m.cpuType + "\n";
	conf += "\n[dosbox]\n";
	if (m.videoCardType != "auto") conf += "machine = " + m.videoCardType + "\n";

	if (!m.extraConf.empty()) {
		conf += "\n";
		conf += m.extraConf;
		conf += "\n";
	}
	return conf;
}

uint64_t dosdrv_formatted_disk(const std::string &name, const uint8_t **zst, size_t *zstLen)
{
	for (size_t i = 0; i < sizeof dosdrv_formatted_disks / sizeof dosdrv_formatted_disks[0]; i++) {
		if (name == dosdrv_formatted_disks[i].name) {
			if (zst) *zst = dosdrv_formatted_disks[i].zst;
			if (zstLen) *zstLen = dosdrv_formatted_disks[i].zstLen;
			return dosdrv_formatted_disks[i].imageSize;
		}
	}
	return 0;
}

// ---- the HDD memory file --------------------------------------------------
#define FAT_SECTOR_SIZE 512
static constexpr char writableHDDDstFile[] = "HardDiskDrive.img";

static bool loadFileIntoMemoryFile(const std::string &srcFile, const std::string &dstFile, const ssize_t dstSize = -1)
{
	auto srcFilePtr = fopen(srcFile.c_str(), "r");
	if (srcFilePtr == nullptr) { fprintf(stderr, "Could not find/read from file: %s\n", srcFile.c_str()); return false; }

	fseek(srcFilePtr, 0L, SEEK_END);
	long srcFileSize = ftell(srcFilePtr);
	fseek(srcFilePtr, 0L, SEEK_SET);

	const auto dstFileSize = std::max(dstSize, (ssize_t)srcFileSize);

	// If size is not divisible by the sector size, it's a bad image
	if (dstFileSize % FAT_SECTOR_SIZE > 0) {
		fprintf(stderr, "Destination file has a non-sector (%d) divisible size: %ld\n", FAT_SECTOR_SIZE, (long)dstFileSize);
		fclose(srcFilePtr);
		return false;
	}

	auto dstFilePtr = _memFileDirectory.fopen(dstFile, "w");
	if (dstFilePtr == NULL) {
		fprintf(stderr, "Could not open mem file for write: %s\n", dstFile.c_str());
		fclose(srcFilePtr);
		return false;
	}

	if (dstFilePtr->resize(dstFileSize) != 0) {
		fprintf(stderr, "Could not resize mem file: %s to %ld bytes\n", dstFile.c_str(), (long)dstFileSize);
		_memFileDirectory.fclose(dstFilePtr);
		fclose(srcFilePtr);
		return false;
	}

	setbuf(srcFilePtr, NULL);

	// Copy in sector-sized chunks
	uint8_t readBuffer[FAT_SECTOR_SIZE];
	size_t totalChunks = (size_t)srcFileSize / FAT_SECTOR_SIZE;
	for (size_t chunkId = 0; chunkId < totalChunks; chunkId++) {
		auto readBytes = fread(readBuffer, 1, FAT_SECTOR_SIZE, srcFilePtr);
		auto writtenBytes = jaffarCommon::file::MemoryFile::fwrite(readBuffer, 1, FAT_SECTOR_SIZE, dstFilePtr);
		if (readBytes != (size_t)writtenBytes) {
			fprintf(stderr, "Could not write data into mem file: %s\n", dstFile.c_str());
			_memFileDirectory.fclose(dstFilePtr);
			fclose(srcFilePtr);
			return false;
		}
	}
	auto remainder = (size_t)srcFileSize % FAT_SECTOR_SIZE;
	if (remainder > 0) {
		auto readBytes = fread(readBuffer, 1, remainder, srcFilePtr);
		auto writtenBytes = jaffarCommon::file::MemoryFile::fwrite(readBuffer, 1, remainder, dstFilePtr);
		if (readBytes != (size_t)writtenBytes) {
			fprintf(stderr, "Could not write data into mem file: %s\n", dstFile.c_str());
			_memFileDirectory.fclose(dstFilePtr);
			fclose(srcFilePtr);
			return false;
		}
	}

	_memFileDirectory.fclose(dstFilePtr);
	fclose(srcFilePtr);
	return true;
}

// Seeds the HDD memory file from an in-memory zstd head, grown to dstSize.
static bool loadZstIntoMemoryFile(const uint8_t *zst, size_t zstLen, const std::string &dstFile, uint64_t dstSize)
{
	if (dstSize % FAT_SECTOR_SIZE != 0) {
		fprintf(stderr, "formatted disk size not sector divisible: %llu\n", (unsigned long long)dstSize);
		return false;
	}
	unsigned long long headLen = ZSTD_getFrameContentSize(zst, zstLen);
	if (headLen == ZSTD_CONTENTSIZE_ERROR || headLen == ZSTD_CONTENTSIZE_UNKNOWN || headLen > dstSize) {
		fprintf(stderr, "formatted disk head is not readable zstd\n");
		return false;
	}
	std::vector<uint8_t> head((size_t)headLen);
	size_t got = ZSTD_decompress(head.data(), head.size(), zst, zstLen);
	if (ZSTD_isError(got) || got != headLen) {
		fprintf(stderr, "formatted disk head failed to decompress\n");
		return false;
	}
	auto dst = _memFileDirectory.fopen(dstFile, "w");
	if (dst == NULL) return false;
	if (dst->resize((ssize_t)dstSize) != 0) { _memFileDirectory.fclose(dst); return false; }
	auto written = jaffarCommon::file::MemoryFile::fwrite(head.data(), 1, head.size(), dst);
	_memFileDirectory.fclose(dst);
	return (size_t)written == head.size();
}

uint8_t *dosdrv_hdd_buffer()
{
	if (!_memFileDirectory.contains(writableHDDDstFile)) return nullptr;
	return _memFileDirectory.getFileBuffer(writableHDDDstFile);
}
uint64_t dosdrv_hdd_size()
{
	// getFileSize is -1 for an absent file; no HDD means size 0 here
	if (!_memFileDirectory.contains(writableHDDDstFile)) return 0;
	return (uint64_t)_memFileDirectory.getFileSize(writableHDDDstFile);
}

// ---- video: copied on demand when the render path says so -----------------
static uint32_t *_videoBuffer = nullptr;
static size_t _videoBufferSize = 0;
static int _videoWidth = 0;
static int _videoHeight = 0;

// Turbo. DOSBox-X has its own render kill switch - render.disablerender, the
// one behind its headless mode - and it is NOT usable here: switching it on
// mid-run leaves the machine somewhere else (measured, on the boot leg:
// conventional memory and physical RAM both diverge). The RENDER layer is
// wired into the VGA's event scheduling, so refusing a frame is a decision the
// emulated hardware notices. What is left, and is certainly safe, is the copy
// out: the finished surface never reaches this core's frame buffer.
static bool _render = true;

void dosdrv_set_rendering(bool on)
{
	// Coming back, ask the RENDER layer to treat every line as changed. It
	// skips a row whose bytes match the last one it drew, and while turbo was
	// on it drew none - so without this the frontend would keep showing the
	// picture from before the gap until something on screen happened to move.
	// It is renderer bookkeeping, not machine state: the gate's turbo leg
	// compares every memory domain and finds them identical.
	if (on && !_render) render.scale.clearCache = true;
	_render = on;
}

void doRenderUpdateCallback()
{
	if (!_render) return;
	bool allocateBuffer = false;
	if (sdl.surface->w != _videoWidth) { allocateBuffer = true; _videoWidth = sdl.surface->w; }
	if (sdl.surface->h != _videoHeight) { allocateBuffer = true; _videoHeight = sdl.surface->h; }

	_videoBufferSize = (size_t)_videoWidth * _videoHeight * sizeof(uint32_t);
	if (allocateBuffer) {
		if (_videoBuffer != nullptr) free(_videoBuffer);
		_videoBuffer = (uint32_t *)malloc(_videoBufferSize);
	}
	memcpy(_videoBuffer, sdl.surface->pixels, _videoBufferSize);
}

// ---- driver state ---------------------------------------------------------
static uint32_t _lastFrameTicks = 0;

bool dosdrv_boot(const DosDrvConfig &cfg, std::string *err)
{
	_composedConf = cfg.confText;

	if (cfg.writableHDDImageSize == 0) {
		printf("No writable hard disk drive selected.\n");
	} else {
		bool result;
		if (!cfg.hddSeedFile.empty()) {
			printf("Creating hard disk drive mem file '%s' -> '%s' (%llu bytes)\n",
				cfg.hddSeedFile.c_str(), writableHDDDstFile, (unsigned long long)cfg.writableHDDImageSize);
			result = loadFileIntoMemoryFile(cfg.hddSeedFile, writableHDDDstFile, (ssize_t)cfg.writableHDDImageSize);
		} else {
			printf("Creating formatted hard disk drive mem file '%s' (%llu bytes)\n",
				writableHDDDstFile, (unsigned long long)cfg.writableHDDImageSize);
			result = loadZstIntoMemoryFile(cfg.hddSeedZst, cfg.hddSeedZstLen,
				writableHDDDstFile, cfg.writableHDDImageSize);
		}
		if (!result || !_memFileDirectory.contains(writableHDDDstFile)) {
			if (err) *err = "could not create the hard disk drive mem file";
			return false;
		}
	}

	// Dummy SDL drivers: the machine renders and mixes into memory
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	setenv("SDL_AUDIODRIVER", "dummy", 1);

	printf("Starting DOSBox-X coroutine...\n");
	_driverCoroutine = co_active();
	constexpr size_t stackSize = 4 * 1024 * 1024;
	_emuCoroutine = co_create(stackSize, runMain);
	co_switch(_emuCoroutine); // runs _main until dosbox's first yield

	stick[0].enabled = cfg.joystick1Enabled;
	stick[1].enabled = cfg.joystick2Enabled;
	for (int i = 0; i < 2; i++) {
		stick[i].xpos = 0.0;
		stick[i].ypos = 0.0;
		stick[i].button[0] = false;
		stick[i].button[1] = false;
	}

	user_cursor_locked = true;

	ticksTarget = 0.0;
	_ticksElapsed = 0;
	return true;
}

void dosdrv_frame(const DosDrvInput &f)
{
	_driveUsed = false;

	// Keyboard: diff against the previous frame into press/release sets
	_releasedKeys.clear();
	_pressedKeys.clear();
	std::set<KBD_KEYS> newPressedKeys;
	for (int i = 0; i < DOSDRV_KEY_COUNT; i++) {
		auto key = (KBD_KEYS)i;
		bool wasPressed = _prevPressedKeys.find(key) != _prevPressedKeys.end();
		if (f.keys[i] != 0) {
			if (!wasPressed) _pressedKeys.insert(key);
			newPressedKeys.insert(key);
		} else if (wasPressed) {
			_releasedKeys.insert(key);
		}
	}
	_prevPressedKeys = newPressedKeys;

	// Drive swapping
	if (f.insertFloppyDisk >= 0) {
		printf("Swapping to Floppy Disk: %d\n", f.insertFloppyDisk);
		swapInDrive(DOS_DRIVE_A, (unsigned)f.insertFloppyDisk + 1); // 0 is A:
	}
	if (f.insertCDROM >= 0) {
		printf("Swapping to CDROM: %d\n", f.insertCDROM);
		swapInDrive(DOS_DRIVE_D, (unsigned)f.insertCDROM + 1); // 3 is D:
	}

	// Joysticks
	for (int i = 0; i < 2; i++) {
		const DosDrvJoystick &j = i == 0 ? f.joy1 : f.joy2;
		if (!stick[i].enabled) continue;
		stick[i].xpos = 0.0;
		stick[i].ypos = 0.0;
		if (j.up) stick[i].ypos = -1.0f;
		if (j.down) stick[i].ypos = 1.0f;
		if (j.left) stick[i].xpos = -1.0f;
		if (j.right) stick[i].xpos = 1.0f;
		stick[i].button[0] = j.button1;
		stick[i].button[1] = j.button2;
	}

	// Mouse
	if (f.mouse.speedX != 0 || f.mouse.speedY != 0) {
		mouse.x = (double)mouse.min_x + ((double)f.mouse.posX / (double)MOUSE_MAX_X) * (double)mouse.max_x;
		mouse.y = (double)mouse.min_y + ((double)f.mouse.posY / (double)MOUSE_MAX_Y) * (double)mouse.max_y;

		float adjustedDeltaX = (float)f.mouse.speedX * f.mouse.sensitivity;
		float adjustedDeltaY = (float)f.mouse.speedY * f.mouse.sensitivity;

		float dx = adjustedDeltaX * mouse.pixelPerMickey_x;
		float dy = adjustedDeltaY * mouse.pixelPerMickey_y;

		mouse.mickey_x = adjustedDeltaX * mouse.mickeysPerPixel_x;
		mouse.mickey_y = adjustedDeltaY * mouse.mickeysPerPixel_y;

		mouse.mickey_accum_x += (dx * mouse.mickeysPerPixel_x);
		mouse.mickey_accum_y += (dy * mouse.mickeysPerPixel_y);

		mouse.ps2x += adjustedDeltaX;
		mouse.ps2y += adjustedDeltaY;
		if (mouse.ps2x >= 32768.0) mouse.ps2x -= 65536.0;
		else if (mouse.ps2x <= -32769.0) mouse.ps2x += 65536.0;
		if (mouse.ps2y >= 32768.0) mouse.ps2y -= 65536.0;
		else if (mouse.ps2y <= -32769.0) mouse.ps2y += 65536.0;

		Mouse_AddEvent(MOUSE_HAS_MOVED);
	}
	if (f.mouse.leftPressed) Mouse_ButtonPressed(0);
	if (f.mouse.middlePressed) Mouse_ButtonPressed(2);
	if (f.mouse.rightPressed) Mouse_ButtonPressed(1);
	if (f.mouse.leftReleased) Mouse_ButtonReleased(0);
	if (f.mouse.middleReleased) Mouse_ButtonReleased(2);
	if (f.mouse.rightReleased) Mouse_ButtonReleased(1);

	_audioSamples.clear();

	// Slice emulated time: run dosbox until this frame's tick target
	double fps = (double)f.framerateNumerator / (double)f.framerateDenominator;
	double ticksPerFrame = 1000.0 / fps;
	auto t0 = _ticksElapsed;
	ticksTarget += ticksPerFrame;
	while (_ticksElapsed < (uint32_t)ticksTarget) co_switch(_emuCoroutine);
	_lastFrameTicks = _ticksElapsed - t0;
}

const uint32_t *dosdrv_video(int *w, int *h)
{
	if (w) *w = _videoWidth;
	if (h) *h = _videoHeight;
	return _videoBuffer;
}

const int16_t *dosdrv_audio(int *sample_pairs)
{
	if (sample_pairs) *sample_pairs = (int)(_audioSamples.size() / 2);
	return _audioSamples.data();
}

uint32_t dosdrv_last_frame_ticks() { return _lastFrameTicks; }
uint32_t dosdrv_ticks_elapsed() { return _ticksElapsed; }

void dosdrv_refresh_rate(int *numerator, int *denominator)
{
	if (numerator) *numerator = _refreshRateNumerator;
	if (denominator) *denominator = _refreshRateDenominator;
}

bool dosdrv_drive_activity() { return _driveUsed; }
bool dosdrv_input_was_read() { return true; } // no lag concept yet (as in BizHawk)

// ---- memory domains -------------------------------------------------------
#define DOS_CONVENTIONAL_MEMORY_SIZE (640 * 1024)
#define DOS_UPPER_MEMORY_SIZE (384 * 1024)
#define DOS_LOWER_MEMORY_SIZE (DOS_CONVENTIONAL_MEMORY_SIZE + DOS_UPPER_MEMORY_SIZE)

bool dosdrv_domain(int index, const char **name, uint8_t **data, uint64_t *size, bool *writable)
{
	const char *dn = nullptr;
	uint8_t *dd = nullptr;
	uint64_t ds = 0;
	bool dw = true;
	int i = 0;

	if (index == i++) { dn = "Conventional Memory"; dd = MemBase; ds = DOS_CONVENTIONAL_MEMORY_SIZE; }
	else if (index == i++) { dn = "Upper Memory Area"; dd = &MemBase[DOS_CONVENTIONAL_MEMORY_SIZE]; ds = DOS_UPPER_MEMORY_SIZE; }
	else if (index == i++) {
		int64_t highMemSize = (int64_t)MemSize - DOS_LOWER_MEMORY_SIZE;
		dn = "Extended Memory";
		if (highMemSize > 0) { dd = &MemBase[DOS_LOWER_MEMORY_SIZE]; ds = (uint64_t)highMemSize; }
		else { dd = MemBase; ds = 0; }
	}
	else if (index == i++) { dn = "Physical RAM"; dd = MemBase; ds = MemSize; }
	else if (index == i++) { dn = "Video RAM"; dd = vga.mem.linear; ds = vga.mem.memsize; }
	else if (index == i++) {
		if (dosdrv_hdd_size() == 0) return false;
		dn = "Hard Disk Drive"; dd = dosdrv_hdd_buffer(); ds = dosdrv_hdd_size();
	}
	else return false;

	if (name) *name = dn;
	if (data) *data = dd;
	if (size) *size = ds;
	if (writable) *writable = dw;
	return true;
}
