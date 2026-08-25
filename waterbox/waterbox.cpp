// The chimera guest ABI layer: wraps dosbox-driver into the miniBox core ABI
// (the same export surface as the other chimera cores). Compiled ONLY for the
// guest; run-native.cpp is the native twin.
//
// Input: DOSBox's keyboard alone is 101 keys, far past the packed u64, so the
// per-frame input rides a guest-memory block instead - the host fills the
// WbxInput struct (GetInputBuffer) before each FrameAdvance and the packed
// argument is ignored. The frontend-side wide-input extension will drive the
// same block (docs/PLAN.md).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <emulibc.h>

#include "dosbox-driver.h"
#include "waterbox-input.h"

static char g_loadError[512];
static bool g_inited;
static WbxInput g_input;

extern "C" {

ECL_EXPORT const char *GetLoadError(void) { return g_loadError; }

ECL_EXPORT int Init(void)
{
	g_loadError[0] = '\0';
	g_input.insertFloppyDisk = -1; // 0 would mean "swap to image 0"
	g_input.insertCDROM = -1;

	// The boot channel: a small key=value text the host mounts as "drvconfig"
	// (the composed dosbox-x.conf is mounted separately under its own name,
	// where dosbox itself reads it from the working directory).
	DosDrvConfig cfg;
	if (FILE *f = fopen("drvconfig", "rb")) {
		char line[256];
		while (fgets(line, sizeof line, f)) {
			unsigned long long v = 0;
			if (sscanf(line, "hddSize=%llu", &v) == 1) cfg.writableHDDImageSize = v;
			else if (sscanf(line, "joy1=%llu", &v) == 1) cfg.joystick1Enabled = v != 0;
			else if (sscanf(line, "joy2=%llu", &v) == 1) cfg.joystick2Enabled = v != 0;
		}
		fclose(f);
	}

	std::string err;
	if (!dosdrv_boot(cfg, &err)) {
		snprintf(g_loadError, sizeof g_loadError, "%s", err.c_str());
		printf("chimera-dosbox-x: cannot boot: %s\n", err.c_str());
		return 0;
	}
	g_inited = true;
	return 1;
}

ECL_EXPORT WbxInput *GetInputBuffer(void) { return &g_input; }

ECL_EXPORT void FrameAdvance(uint64_t)
{
	if (!g_inited)
		return;

	DosDrvInput in;
	memcpy(in.keys, g_input.keys, WBX_KEY_COUNT);
	auto joy = [](const WbxJoy &j, DosDrvJoystick &o) {
		o.up = j.up != 0; o.down = j.down != 0; o.left = j.left != 0; o.right = j.right != 0;
		o.button1 = j.button1 != 0; o.button2 = j.button2 != 0;
	};
	joy(g_input.joy1, in.joy1);
	joy(g_input.joy2, in.joy2);
	in.mouse.posX = g_input.mouse.posX;
	in.mouse.posY = g_input.mouse.posY;
	in.mouse.speedX = g_input.mouse.speedX;
	in.mouse.speedY = g_input.mouse.speedY;
	in.mouse.leftPressed = g_input.mouse.leftPressed != 0;
	in.mouse.middlePressed = g_input.mouse.middlePressed != 0;
	in.mouse.rightPressed = g_input.mouse.rightPressed != 0;
	in.mouse.leftReleased = g_input.mouse.leftReleased != 0;
	in.mouse.middleReleased = g_input.mouse.middleReleased != 0;
	in.mouse.rightReleased = g_input.mouse.rightReleased != 0;
	in.mouse.sensitivity = g_input.mouse.sensitivity != 0.0f ? g_input.mouse.sensitivity : 1.0f;
	in.insertFloppyDisk = g_input.insertFloppyDisk;
	in.insertCDROM = g_input.insertCDROM;
	if (g_input.framerateNumerator > 0 && g_input.framerateDenominator > 0) {
		in.framerateNumerator = g_input.framerateNumerator;
		in.framerateDenominator = g_input.framerateDenominator;
	}

	// One-shot actions: the host rearms them when it means them again
	g_input.insertFloppyDisk = -1;
	g_input.insertCDROM = -1;

	dosdrv_frame(in);
}

ECL_EXPORT uint32_t *GetVideoBgra(void)
{
	int w, h;
	return const_cast<uint32_t *>(dosdrv_video(&w, &h));
}
ECL_EXPORT int GetVideoWidth(void)
{
	int w = 0, h = 0;
	dosdrv_video(&w, &h);
	return w;
}
ECL_EXPORT int GetVideoHeight(void)
{
	int w = 0, h = 0;
	dosdrv_video(&w, &h);
	return h;
}

ECL_EXPORT int16_t *GetAudio(void)
{
	int n = 0;
	return const_cast<int16_t *>(dosdrv_audio(&n));
}
ECL_EXPORT int GetAudioSampleCount(void)
{
	int n = 0;
	dosdrv_audio(&n);
	return n;
}

// The DOS video mode's own refresh rate when the VGA emulation has reported
// one, else the classic 70.086 Hz text mode (3146888/44900).
ECL_EXPORT int GetVsyncNumerator(void)
{
	int n = 0, d = 0;
	dosdrv_refresh_rate(&n, &d);
	return n > 0 && d > 0 ? n : 3146888;
}
ECL_EXPORT int GetVsyncDenominator(void)
{
	int n = 0, d = 0;
	dosdrv_refresh_rate(&n, &d);
	return n > 0 && d > 0 ? d : 44900;
}

ECL_EXPORT uint32_t GetTicksElapsed(void) { return dosdrv_ticks_elapsed(); }
ECL_EXPORT int GetDriveActivityFlag(void) { return dosdrv_drive_activity() ? 1 : 0; }

// ---- memory domains ----

ECL_EXPORT int GetMemoryDomainCount(void)
{
	int n = 0;
	while (dosdrv_domain(n, nullptr, nullptr, nullptr, nullptr)) n++;
	return n;
}
ECL_EXPORT const char *GetMemoryDomainName(int i)
{
	const char *name = nullptr;
	dosdrv_domain(i, &name, nullptr, nullptr, nullptr);
	return name;
}
ECL_EXPORT uint8_t *GetMemoryDomainPtr(int i)
{
	uint8_t *data = nullptr;
	dosdrv_domain(i, nullptr, &data, nullptr, nullptr);
	return data;
}
ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
	uint64_t size = 0;
	dosdrv_domain(i, nullptr, nullptr, &size, nullptr);
	return (int64_t)size;
}
ECL_EXPORT int GetMemoryDomainWritable(int i)
{
	bool w = false;
	return dosdrv_domain(i, nullptr, nullptr, nullptr, &w) && w ? 1 : 0;
}

// ---- savedata export (the sixth optional guest ABI group) ----
// The writable hard disk image is this core's save data (chimera
// docs/save-data.md): it lives in guest memory (the sealed baseline carries
// the seed; savestates only the dirtied pages), and this group is the user's
// way OUT - one file, the whole image.

ECL_EXPORT int32_t GetSaveDataFileCount(void) { return dosdrv_hdd_size() != 0 ? 1 : 0; }
ECL_EXPORT const char *GetSaveDataFileName(int32_t i) { return i == 0 ? "HardDiskDrive.img" : nullptr; }
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i) { return i == 0 ? (int64_t)dosdrv_hdd_size() : 0; }
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i) { return i == 0 ? dosdrv_hdd_buffer() : nullptr; }

}  // extern "C"
