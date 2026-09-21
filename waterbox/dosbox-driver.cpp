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
#include <map>
#include <set>
#include <string>
#include <vector>

#include <jaffarCommon/file.hpp>

#include "sparse-disk.h"

#include "dosbox_conf_assets.h" // generated: base conf + formatted disk heads + fonts
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

// ---- the in-guest file directory (floppies and CDs live here) -------------
jaffarCommon::file::MemoryFileDirectory _memFileDirectory;

// ---- the writable hard disk, which does NOT (see sparse-disk.h) -----------
SparseDisk _sparseHardDisk;
std::string _sparseHardDiskName;

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

// ---- drive activity, one flag per KIND OF MEDIA ---------------------------
// Set wherever a sector is actually read or written - the CD emulation in
// cdrom_image.cpp and drive_iso.cpp, the disk emulation in bios_disk.cpp,
// drive_fat.cpp and bios_vhd.cpp - and cleared at the top of every frame, so
// what the frontend reads is "was this drive touched during the frame just
// run" and nothing longer-lived than that.
bool _cdDriveUsed = false;
bool _diskDriveUsed = false;
bool _floppyDriveUsed = false; // drive_fat.cpp tells a floppy from the hard disk

// ---- configuration composition ---------------------------------------------

static std::string _composedConf;

// setup.cpp's ParseConfigFile reads this when it cannot open the file
extern "C" const char *chimera_composed_conf()
{
	return _composedConf.empty() ? nullptr : _composedConf.c_str();
}

// The free fonts DOSBox-X looks for by name (FREECG98.BMP for PC-98 text, Unifont
// as FONTX2 for DOS/V and JEGA), served from the binary: the sandbox has no
// writable file system to put them in. DOSBox-X asks here only after the work
// directory (patches: int10.cpp, int_dosv.cpp), so a real NEC FONT.ROM - the
// pc98FontRom firmware - or a font the user's conf names still wins.
extern "C" FILE *chimera_bundled_file(const char *name)
{
	const char *base = strrchr(name, '/');
	base = base ? base + 1 : name;
	for (const auto &font : dosdrv_fonts)
		if (strcmp(base, font.name) == 0)
			return fmemopen(const_cast<unsigned char *>(font.data), font.len, "rb");
	return nullptr;
}

// ---- .dcp floppy images ----------------------------------------------------
//
// DCP is a PC-98 dump format (the "DiskCopy" of Japanese doujin/net dumps):
// a 162-byte header - one byte of media type, 160 bytes of per-track "this
// track is in the file" flags, one "every track is in the file" flag - and
// then the tracks that are present, in order, as plain sector runs. A track
// not in the file reads as 0xE5. Nothing in DOSBox-X reads it, and its sector
// layer wants a flat image, so a .dcp is DECODED when it is opened: the flat
// image is built once in memory (one buffer per name, so a mount and a boot
// share what they write) and every open of that name is an fmemopen over it.
// The result is exactly the raw image the same disk would be as .hdm, which
// DOSBox-X types by size (1232K for the 1.25 MB format that every PC-98 game
// disk is), and writes land in the buffer for the session. The layouts are
// the ones Neko Project II accepts (fdd_head_dcp.h); the N88-BASIC ones
// (0x11, 0x19, 0x21) are not DOS disks and are refused.
namespace {
struct DcpFormat { uint8_t media; uint16_t tracks; uint16_t sectors; uint16_t sectorSize; };
const DcpFormat dcpFormats[] = {
	{ 0x01, 154,  8, 1024 }, // 2HD  8 sectors, 1.25 MB - the PC-98 standard
	{ 0x02, 160, 15,  512 }, // 2HD 15 sectors, 1.21 MB
	{ 0x03, 160, 18,  512 }, // 2HQ 18 sectors, 1.44 MB
	{ 0x04, 160,  8,  512 }, // 2DD  8 sectors, 640 KB
	{ 0x05, 160,  9,  512 }, // 2DD  9 sectors, 720 KB
	{ 0x08, 154,  9, 1024 }, // 2HD  9 sectors
};
const size_t DCP_HEADER = 162;
std::map<std::string, std::vector<uint8_t>> _dcpImages;

bool endsWithNoCase(const char *name, const char *ext)
{
	const size_t n = strlen(name), e = strlen(ext);
	return n >= e && strcasecmp(name + n - e, ext) == 0;
}
} // namespace

// fopen_lock (dos_programs.cpp) asks here first; NULL means "not a .dcp, open
// it yourself". `readonly` is cleared: the decoded image takes writes.
extern "C" FILE *chimera_dcp_open(const char *name, bool *readonly)
{
	if (name == NULL || !endsWithNoCase(name, ".dcp")) return NULL;
	auto it = _dcpImages.find(name);
	if (it == _dcpImages.end()) {
		FILE *f = fopen(name, "rb");
		if (f == NULL) return NULL;
		uint8_t head[DCP_HEADER];
		if (fread(head, 1, sizeof head, f) != sizeof head) { fclose(f); return NULL; }
		const DcpFormat *fmt = NULL;
		for (const DcpFormat &d : dcpFormats) if (d.media == head[0]) fmt = &d;
		if (fmt == NULL) {
			fprintf(stderr, "dcp: %s: media type 0x%02x is not a DOS disk this core reads\n", name, head[0]);
			fclose(f);
			return NULL;
		}
		const size_t trackSize = (size_t)fmt->sectors * fmt->sectorSize;
		std::vector<uint8_t> image((size_t)fmt->tracks * trackSize, 0xE5);
		const bool all = head[161] == 0x01;
		size_t present = 0;
		for (unsigned t = 0; t < fmt->tracks; t++) {
			if (!all && head[1 + t] != 0x01) continue;
			if (fread(image.data() + t * trackSize, 1, trackSize, f) != trackSize) {
				fprintf(stderr, "dcp: %s: track %u is missing from the file\n", name, t);
				fclose(f);
				return NULL;
			}
			present++;
		}
		fclose(f);
		fprintf(stderr, "dcp: %s: media 0x%02x, %zu of %u tracks in the file, %zu-byte image\n",
			name, head[0], present, fmt->tracks, image.size());
		it = _dcpImages.emplace(name, std::move(image)).first;
	}
	if (readonly) *readonly = false;
	return fmemopen(it->second.data(), it->second.size(), "r+b");
}

static bool wantsDosvFonts(const DosDrvMachine &m) { return m.videoCardType == "jega" || m.extraConf.find("dosv") != std::string::npos; }

std::string dosdrv_compose_conf(const DosDrvMachine &m)
{
	std::string conf((const char *)dosdrv_conf_base, dosdrv_conf_base_len);
	conf += "\n";
	// base.conf and then the SETTINGS - nothing between them. There used to be
	// a machine-preset .conf blob here, appended after base.conf and before
	// these sections, which meant it overrode the user for every key the
	// settings did not re-state. The presets are declared to the frontend now
	// (waterbox.config "presets"), which resolves them into these very
	// settings before the core is asked for anything.
	//
	// The section order below still mirrors the author's BizHawk integration
	// (DOSBox.cs's configuration composition), so the same settings produce
	// the same machine and a finished BizHawk movie stays convertible; the
	// sections the ex-preset keys need ([video], [dos], the disk controllers)
	// had no counterpart there and go after them.
	conf += "[joystick]\njoysticktype = ";
	conf += (m.joystick1 || m.joystick2) ? "2axis\n" : "none\n";
	conf += "[speaker]\n";
	// base.conf's own spelling of this bool. The BizHawk composition wrote
	// "Enabled"/"Disabled" here, which DOSBox-X's bool parser accepts as the
	// same thing (Value::set_bool lowcases and takes enabled/disabled), but
	// saying it the way the conf it overrides says it lets the composed text be
	// compared against base.conf line for line.
	conf += m.pcSpeaker == "disabled" ? "pcspeaker = false\n" : "pcspeaker = true\n";
	conf += "\n[sblaster]\n";
	conf += "sbtype = " + m.soundBlasterModel + "\n";
	if (m.soundBlasterIRQ != -1) conf += "irq = " + std::to_string(m.soundBlasterIRQ) + "\n";
	conf += "\n[dosbox]\n";
	conf += "memsize = " + std::to_string(m.memsizeMB) + "\n";
	conf += "memsizekb = " + std::to_string(m.memsizeKB) + "\n";

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
		static const char *floppyExts[] = { ".ima", ".img", ".xdf", ".fdi", ".hdm", ".nfd", ".d88", ".dcp" };
		for (const char *e : floppyExts) {
			if (m.romExt == e) { conf += "imgmount a rom" + extras + " -t floppy\n"; break; }
		}
		if (m.romExt == ".iso" || m.romExt == ".cue") {
			conf += "imgmount d rom" + extras + " -t iso\n";
		}
	}
	if (m.hddMounted) conf += m.hddIsHdi ? "imgmount c HardDiskDrive.hdi\n" : "imgmount c HardDiskDrive.img\n";
	if (m.bootDrive == "a" || m.bootDrive == "c") {
		// the very last autoexec line: boot never returns to the shell
		// (a chimera addition; BizHawk movies run with bootDrive none)
		conf += "boot " + m.bootDrive + ":\n";
	}

	// BizHawk emits [cpu] and the machine override AFTER the autoexec
	conf += "\n[cpu]\n";
	// ALWAYS a fixed count. DOSBox-X's own "auto" and "max" chase the host's
	// speed, which is a different machine on every PC and not a machine a movie
	// can be replayed on.
	conf += "cycles = fixed " + std::to_string(m.cpuCycles) + "\n";
	conf += "cputype = " + m.cpuType + "\n";
	conf += "core = " + m.cpuCore + "\n";
	conf += "\n[dosbox]\n";
	conf += "machine = " + m.videoCardType + "\n";

	// ---- the rest of the machine the ten presets describe -------------------
	conf += "\n[video]\n";
	conf += "vmemsize = " + std::to_string(m.videoMemoryMB) + "\n";
	conf += "vesa modelist width limit = " + std::to_string(m.vesaWidthLimit) + "\n";
	conf += "vesa modelist height limit = " + std::to_string(m.vesaHeightLimit) + "\n";
	conf += "\n[dos]\n";
	// "auto" is what an unset ver means, so it is left unset rather than
	// spelled out - base.conf's own empty value already says it.
	if (m.dosVersion != "auto") conf += "ver = " + m.dosVersion + "\n";
	conf += "hard drive data rate limit = " + std::to_string(m.hardDriveDataRate) + "\n";
	conf += "floppy drive data rate limit = " + std::to_string(m.floppyDriveDataRate) + "\n";
	{
		// One setting, five keys: Windows 3.11's and Windows 95's 32-bit disk
		// access needs the BIOS calls to move the controllers' registers and
		// raise v86-mode traps, on the IDE channels and on the floppy
		// controller alike. Nothing else in DOSBox-X reads them.
		const char *f = m.int13FakeIo ? "true" : "false";
		conf += "\n[fdc, primary]\nint13fakev86io = " + std::string(f) + "\n";
		conf += "\n[ide, primary]\nint13fakeio = " + std::string(f) + "\n";
		conf += "int13fakev86io = " + std::string(f) + "\n";
		conf += "\n[ide, secondary]\nint13fakeio = " + std::string(f) + "\n";
		conf += "int13fakev86io = " + std::string(f) + "\n";
		// the CD-ROM lives on the secondary channel, as it did on the hardware
		conf += "cd-rom insertion delay = " + std::to_string(m.cdromInsertionDelayMs) + "\n";
	}

	// ---- devices that need their ROM (chimera additions; silent when unset,
	// so a BizHawk movie's composition is byte for byte what it was) ---------
	// Every path is the work directory: that is where a firmware is mounted,
	// under the very name DOSBox-X looks for.
	if (m.midiDevice != "none") {
		// munt renders on the emulation thread (mt32.thread = false in the
		// base conf): a second thread would make the mix a race.
		conf += "\n[midi]\nmpu401 = intelligent\nmididevice = mt32\nmt32.romdir = ./\n";
		conf += std::string("mt32.model = ") + (m.midiDevice == "cm32l" ? "cm32l" : "mt32") + "\n";
		conf += "mt32.thread = false\n";
	}
	if (m.pc98FontRom || m.pc98SoundBios) {
		conf += "\n[pc98]\n";
		if (m.pc98FontRom) conf += "pc-98 try font rom = true\n";
		if (m.pc98SoundBios) conf += "pc-98 sound bios = true\npc-98 load sound bios rom file = true\n";
	}
	// before the user's conf, which may name fonts of its own
	if (wantsDosvFonts(m))
		conf += "\n[dosv]\nfontxsbcs16 = UnifontExMonoAnk.fontx2\nfontxdbcs = UnifontExMonoKanji.fontx2\n";
	if (m.ibmRomBasic) conf += "\n[dosbox]\nibm rom basic = IBMBASIC.ROM\n";
	if (m.vgaBiosRom) conf += "\n[video]\nvga bios use rom image = true\nvga bios rom image = VGABIOS.BIN\n";

	if (!m.extraConf.empty()) {
		conf += "\n";
		conf += m.extraConf;
		conf += "\n";
	}
	return conf;
}

// ONE table of what a declared setting name means, so the guest's settings
// channel and run-native's --setting cannot disagree about a machine. A value
// arrives as text because that is what both sides have; the declaration
// (waterbox.config) is what typed and bounded it before it got here.
bool dosdrv_machine_setting(DosDrvMachine &m, const std::string &name, const std::string &value)
{
	auto asInt = [&](int32_t &dst) { dst = (int32_t)strtol(value.c_str(), nullptr, 10); return true; };
	auto asBool = [&](bool &dst) {
		dst = value == "true" || value == "True" || value == "1";
		return true;
	};
	if (name == "videoCardType") { m.videoCardType = value; return true; }
	if (name == "memsizeMB") return asInt(m.memsizeMB);
	if (name == "memsizeKB") return asInt(m.memsizeKB);
	if (name == "cpuType") { m.cpuType = value; return true; }
	if (name == "cpuCycles") return asInt(m.cpuCycles);
	if (name == "cpuCore") { m.cpuCore = value; return true; }
	if (name == "soundBlasterModel") { m.soundBlasterModel = value; return true; }
	if (name == "soundBlasterIRQ") return asInt(m.soundBlasterIRQ);
	if (name == "videoMemoryMB") return asInt(m.videoMemoryMB);
	if (name == "vesaModelistWidthLimit") return asInt(m.vesaWidthLimit);
	if (name == "vesaModelistHeightLimit") return asInt(m.vesaHeightLimit);
	if (name == "dosVersion") { m.dosVersion = value; return true; }
	if (name == "hardDriveDataRateLimit") return asInt(m.hardDriveDataRate);
	if (name == "floppyDriveDataRateLimit") return asInt(m.floppyDriveDataRate);
	if (name == "int13FakeIo") return asBool(m.int13FakeIo);
	if (name == "cdromInsertionDelayMs") return asInt(m.cdromInsertionDelayMs);
	if (name == "pcSpeaker") { m.pcSpeaker = value; return true; }
	if (name == "bootDrive") { m.bootDrive = value; return true; }
	if (name == "midiDevice") { m.midiDevice = value; return true; }
	if (name == "pc98FontRom") return asBool(m.pc98FontRom);
	if (name == "pc98SoundBios") return asBool(m.pc98SoundBios);
	if (name == "ibmRomBasic") return asBool(m.ibmRomBasic);
	if (name == "vgaBiosRom") return asBool(m.vgaBiosRom);
	if (name == "joystick1Enabled") return asBool(m.joystick1);
	if (name == "joystick2Enabled") return asBool(m.joystick2);
	return false;
}

void dosdrv_media_counts(const DosDrvMachine &m, int32_t *floppies, int32_t *cds)
{
	int32_t f = 0, c = 0;
	if (!m.floppyImages.empty() || !m.cdImages.empty()) {
		f = (int32_t)m.floppyImages.size();
		c = (int32_t)m.cdImages.size();
	} else {
		/* the extras convention: the rom and rom2..romN all mount on ONE
		 * drive, chosen by the rom's extension - the same test as above */
		static const char *floppyExts[] = { ".ima", ".img", ".xdf", ".fdi", ".hdm", ".nfd", ".d88", ".dcp" };
		for (const char *e : floppyExts) {
			if (m.romExt == e) { f = 1 + m.extraImageCount; break; }
		}
		if (m.romExt == ".iso" || m.romExt == ".cue") c = 1 + m.extraImageCount;
	}
	if (floppies != nullptr) *floppies = f;
	if (cds != nullptr) *cds = c;
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
// A PC-98 .hdi is mounted under a name that says so: DOSBox-X types a disk by
// its extension - the header with the geometry is read, geometry detection is
// skipped, the FAT layer knows it is a hard disk - and the exported save data
// carries the same name, so it goes back into the hdd slot as what it is.
static constexpr char writableHDDDstFileHdi[] = "HardDiskDrive.hdi";

// The hard disk, seeded from a file the project mounted. Nothing is copied:
// the image stays on the host and is read a chunk at a time as the machine
// asks for it, and only WRITTEN chunks are held in guest memory. That is what
// lets a disk be larger than the sandbox and what keeps a savestate to the size
// of what changed rather than the size of the disk.
static bool openHardDiskFromFile(const std::string &srcFile, uint64_t size, bool hdi)
{
	if (size % FAT_SECTOR_SIZE > 0) {
		fprintf(stderr, "Hard disk image has a non-sector (%d) divisible size: %llu\n",
			FAT_SECTOR_SIZE, (unsigned long long)size);
		return false;
	}
	if (hdi) {
		// the header the disk layer will read (bios_disk.cpp, imageDisk_Sparse):
		// refused here, with the reason, rather than mounted as a disk of no
		// geometry that boots into nothing
		uint8_t head[32] = { 0 };
		FILE *f = fopen(srcFile.c_str(), "rb");
		if (f == NULL || fread(head, 1, sizeof head, f) != sizeof head) {
			if (f) fclose(f);
			fprintf(stderr, "Could not read the .hdi header of %s\n", srcFile.c_str());
			return false;
		}
		fclose(f);
		auto le32 = [&](int at) { return (uint32_t)head[at] | ((uint32_t)head[at + 1] << 8) | ((uint32_t)head[at + 2] << 16) | ((uint32_t)head[at + 3] << 24); };
		const uint32_t ofs = le32(8), hddsize = le32(12), sectorsize = le32(16);
		if (sectorsize == 0 || (sectorsize & (sectorsize - 1)) != 0 || sectorsize < 256 || sectorsize > 1024
			|| ofs == 0 || ofs % sectorsize != 0 || ofs % 1024 != 0 || hddsize < sectorsize || (uint64_t)hddsize + ofs > size + 4096) {
			fprintf(stderr, "%s is not a PC-98 .hdi this core reads (header %u, sector %u, size %u)\n",
				srcFile.c_str(), ofs, sectorsize, hddsize);
			return false;
		}
		printf("PC-98 .hdi: %u-byte header, %u bytes/sector, C/H/S %u/%u/%u\n",
			ofs, sectorsize, le32(28), le32(24), le32(20));
	}
	if (!_sparseHardDisk.openFile(srcFile, size)) {
		fprintf(stderr, "Could not open hard disk image: %s\n", srcFile.c_str());
		return false;
	}
	_sparseHardDiskName = hdi ? writableHDDDstFileHdi : writableHDDDstFile;
	return true;
}

// the name the disk is mounted under, which is the name its export carries
const char *dosdrv_hdd_name() { return _sparseHardDiskName.empty() ? writableHDDDstFile : _sparseHardDiskName.c_str(); }

// ...and one of the package's own formatted disks, which is a small
// decompressed head and then zeros all the way down. The zeros are not stored.
static bool openHardDiskFromZst(const uint8_t *zst, size_t zstLen, uint64_t dstSize)
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
	_sparseHardDisk.openHead(std::move(head), dstSize);
	_sparseHardDiskName = writableHDDDstFile;
	return true;
}

uint64_t dosdrv_hdd_size() { return _sparseHardDisk.size(); }

// The disk as it stands: base where nothing was written, overlay where it was.
// This is what an export is made of, and it is the same answer the machine gets
// when it reads a sector.
bool dosdrv_hdd_read(uint64_t offset, void *dst, size_t len)
{
	return _sparseHardDisk.isOpen() && _sparseHardDisk.read(offset, dst, len);
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
	if (sdl.surface == nullptr || sdl.surface->pixels == nullptr) return;

	const int w = sdl.surface->w, h = sdl.surface->h;
	if (w <= 0 || h <= 0) return;

	// A ROW IS AS LONG AS THE SURFACE SAYS IT IS. This used to copy w*h*4 bytes
	// straight out of surface->pixels, which assumes the pitch is exactly w*4
	// and the surface is 32 bits deep. When the pitch is LARGER that reads the
	// wrong bytes into every row after the first; when it is SMALLER - which is
	// what a surface of any other depth would give - it reads past the end of
	// the surface entirely, and a read that leaves the guest's own memory is
	// the one kind of fault a sandbox cannot contain.
	const size_t rowBytes = (size_t)w * sizeof(uint32_t);
	const size_t pitch = (size_t)sdl.surface->pitch;
	if (pitch < rowBytes) return;   // not 32 bits deep: nothing safe to copy

	bool allocateBuffer = false;
	if (w != _videoWidth) { allocateBuffer = true; _videoWidth = w; }
	if (h != _videoHeight) { allocateBuffer = true; _videoHeight = h; }

	_videoBufferSize = rowBytes * (size_t)h;
	if (allocateBuffer) {
		if (_videoBuffer != nullptr) free(_videoBuffer);
		_videoBuffer = (uint32_t *)malloc(_videoBufferSize);
	}
	// a mode too big for the guest's heap is a black frame, not a write to null
	if (_videoBuffer == nullptr) { _videoWidth = _videoHeight = 0; _videoBufferSize = 0; return; }

	const uint8_t *src = (const uint8_t *)sdl.surface->pixels;
	uint8_t *dst = (uint8_t *)_videoBuffer;
	for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * rowBytes, src + (size_t)y * pitch, rowBytes);
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
			printf("Hard disk '%s' as '%s' (%llu bytes), read on demand\n",
				cfg.hddSeedFile.c_str(), cfg.hddIsHdi ? writableHDDDstFileHdi : writableHDDDstFile, (unsigned long long)cfg.writableHDDImageSize);
			result = openHardDiskFromFile(cfg.hddSeedFile, cfg.writableHDDImageSize, cfg.hddIsHdi);
		} else {
			printf("Formatted hard disk '%s' (%llu bytes), zeros not stored\n",
				writableHDDDstFile, (unsigned long long)cfg.writableHDDImageSize);
			result = openHardDiskFromZst(cfg.hddSeedZst, cfg.hddSeedZstLen, cfg.writableHDDImageSize);
		}
		if (!result || !_sparseHardDisk.isOpen()) {
			if (err) *err = "could not open the hard disk drive image";
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
	_cdDriveUsed = false;
	_diskDriveUsed = false;
	_floppyDriveUsed = false;

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
	//
	// A speed of zero means "move by how far the position moved" - BizHawk's
	// frontend does exactly this before its driver sees the frame (DOSBox.cs:
	// DeltaX = SpeedX != 0 ? SpeedX : PosX - lastPosX, the last position kept
	// in its savestate). It was never ported, so Mouse Position X/Y did nothing
	// on its own (issue #61). It lives here rather than in the guest adapter so
	// that the native reference and the sandbox share it, and the last position
	// is ordinary guest memory, so a savestate carries it as BizHawk's does.
	// Like BizHawk, it is kept whether or not either speed was given.
	static int32_t lastMousePosX = 0, lastMousePosY = 0;
	const int32_t mouseSpeedX = f.mouse.speedX != 0 ? f.mouse.speedX : f.mouse.posX - lastMousePosX;
	const int32_t mouseSpeedY = f.mouse.speedY != 0 ? f.mouse.speedY : f.mouse.posY - lastMousePosY;
	lastMousePosX = f.mouse.posX;
	lastMousePosY = f.mouse.posY;
	if (mouseSpeedX != 0 || mouseSpeedY != 0) {
		mouse.x = (double)mouse.min_x + ((double)f.mouse.posX / (double)MOUSE_MAX_X) * (double)mouse.max_x;
		mouse.y = (double)mouse.min_y + ((double)f.mouse.posY / (double)MOUSE_MAX_Y) * (double)mouse.max_y;

		float adjustedDeltaX = (float)mouseSpeedX * f.mouse.sensitivity;
		float adjustedDeltaY = (float)mouseSpeedY * f.mouse.sensitivity;

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

bool dosdrv_cd_activity() { return _cdDriveUsed; }
bool dosdrv_disk_activity() { return _diskDriveUsed; }
bool dosdrv_floppy_activity() { return _floppyDriveUsed; }
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
	/* The hard disk is NOT a memory domain any more, and cannot be: it does
	 * not live in guest memory, so there is no address to hand over (see
	 * waterbox/sparse-disk.h). It was only ever a domain because it happened
	 * to be a buffer - a disk image is not the machine's memory, and what a
	 * person wants from it is the EXPORT, which is a whole mountable image. */
	else return false;

	if (name) *name = dn;
	if (data) *data = dd;
	if (size) *size = ds;
	if (writable) *writable = dw;
	return true;
}
