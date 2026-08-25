// The chimera DOSBox-X driver: boots the machine, runs it a frame at a time,
// hands out video/audio/memory. Descends from the author's BizHawk-side
// bizhawk.cpp; compiled into BOTH the native reference build (run-native) and
// the guest (waterbox.cpp), so the equivalence gate compares like with like.
//
// DOSBox has no frame loop of its own: _main() runs on a coroutine, the
// driver's virtual clock (_GetTicks/_Delay) replaces SDL's, and a "frame" is
// "run until the tick target for the chosen framerate is reached" (see
// docs/PLAN.md). Everything the machine can change lives in memory reachable
// from here, so whole-machine savestates capture it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

inline constexpr int DOSDRV_KEY_COUNT = 0x65; // KBD_KEYS as the input array size

struct DosDrvConfig {
	// The full composed dosbox-x.conf text (base + machine preset + sections +
	// autoexec + user confs). The driver writes nothing; the HOST places it
	// where dosbox-x will read it (a mounted "dosbox-x.conf" in the guest, a
	// work-directory file natively).
	bool joystick1Enabled = false;
	bool joystick2Enabled = false;
	// Nonzero = mount a writable hard disk: the read-only "HardDiskDrive"
	// file is copied into the in-guest memory file "HardDiskDrive.img"
	// (pre-seal, so savestates only carry dirtied pages), grown to this size.
	uint64_t writableHDDImageSize = 0;
};

struct DosDrvJoystick {
	bool up = false, down = false, left = false, right = false;
	bool button1 = false, button2 = false;
};

struct DosDrvMouse {
	int32_t posX = 0, posY = 0;      // absolute, in the 800x600 driver range
	int32_t speedX = 0, speedY = 0;  // relative movement this frame
	bool leftPressed = false, middlePressed = false, rightPressed = false;
	bool leftReleased = false, middleReleased = false, rightReleased = false;
	float sensitivity = 1.0f;
};

struct DosDrvInput {
	uint8_t keys[DOSDRV_KEY_COUNT] = {}; // 1 = held, indexed by KBD_KEYS
	DosDrvJoystick joy1, joy2;
	DosDrvMouse mouse;
	int32_t insertFloppyDisk = -1; // >= 0: swap drive A to image N this frame
	int32_t insertCDROM = -1;      // >= 0: swap drive D to disc N this frame
	// The frame cadence the frontend runs at; the driver slices emulated time
	// into 1000/fps millisecond ticks. DOS text mode is 70.086 Hz:
	int32_t framerateNumerator = 3146888;
	int32_t framerateDenominator = 44900;
};

// Boots the machine: seeds the HDD memory file, starts the emulation
// coroutine (which runs dosbox-x _main until its first yield). false + err on
// failure. Must be called exactly once, before seal in the guest.
bool dosdrv_boot(const DosDrvConfig &cfg, std::string *err);

// One frame: applies inputs, advances the virtual clock by one frame's ticks.
void dosdrv_frame(const DosDrvInput &input);

// The last frame's picture, BGRA. Size can change with the DOS video mode.
const uint32_t *dosdrv_video(int *w, int *h);
// Interleaved stereo s16 samples produced during the last frame.
const int16_t *dosdrv_audio(int *sample_pairs);
// Emulated milliseconds consumed by the last frame (the cycle count).
uint32_t dosdrv_last_frame_ticks();
// Total emulated milliseconds since boot.
uint32_t dosdrv_ticks_elapsed();
// The DOS video mode's own refresh rate, as reported by the VGA emulation.
void dosdrv_refresh_rate(int *numerator, int *denominator);
// Whether any drive I/O happened during the last frame (the drive light).
bool dosdrv_drive_activity();
// Nonzero while dosbox reads input this frame - the lag-frame signal.
bool dosdrv_input_was_read();

// Memory domains, in a fixed order: 0 = Conventional Memory, 1 = Upper Memory
// Area, 2 = Extended Memory (may be absent), 3 = Physical RAM, 4 = Video RAM,
// 5 = Hard Disk Drive (absent when no HDD is mounted).
// Returns false when index is past the end.
bool dosdrv_domain(int index, const char **name, uint8_t **data, uint64_t *size, bool *writable);

// The writable hard disk image (the savedata export): NULL/0 when none.
uint8_t *dosdrv_hdd_buffer();
uint64_t dosdrv_hdd_size();
