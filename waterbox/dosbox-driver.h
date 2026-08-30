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
#include <vector>

inline constexpr int DOSDRV_KEY_COUNT = 0x67; // KBD_NONE + the 102 keys the wire format carries

struct DosDrvConfig {
	bool joystick1Enabled = false;
	bool joystick2Enabled = false;
	// The writable hard disk (the in-memory "HardDiskDrive.img", seeded
	// pre-seal so savestates only carry dirtied pages): the seed is either a
	// readable file (a mounted .hdd rom) or an embedded zstd disk head, grown
	// to writableHDDImageSize. Zero size = no hard disk.
	uint64_t writableHDDImageSize = 0;
	std::string hddSeedFile;              // "" = use the zst seed instead
	const uint8_t *hddSeedZst = nullptr;  // a dosdrv_formatted_disk head
	size_t hddSeedZstLen = 0;
	// The composed dosbox-x.conf. When dosbox cannot open the file by name
	// (nothing mounted it), Config::ParseConfigFile falls back to this text -
	// so the GUEST composes its own configuration from settings, and a host
	// that stages a real file (run-native's work directory) wins.
	std::string confText;
};

// ---- configuration composition (shared by both builds - the gate compares
// the machines, so the text must be identical by construction) --------------

struct DosDrvMachine {
	std::string machinePreset = "1993_ibm_ps2_53_slc2_486"; // a dosdrv_conf_presets name
	bool joystick1 = true;   // plugged into gameport 1
	bool joystick2 = true;   // plugged into gameport 2
	int32_t memsizeMB = -1;  // -1 = the preset's value
	int32_t cpuCycles = -1;  // -1 = the preset's value
	std::string cpuType = "auto";        // conf cputype when not "auto"
	std::string videoCardType = "auto";  // conf machine= when not "auto"
	std::string pcSpeaker = "auto";      // "disabled"/"enabled" when not "auto"
	std::string soundBlasterModel = "auto"; // conf sbtype when not "auto"
	int32_t soundBlasterIRQ = -1;        // conf irq when not -1
	std::string romExt;      // lowercased extension of the loaded file ("" = none)
	int32_t extraImageCount = 0; // additional images mounted as rom2..romN, joining the drive's swap list
	// project mode (the "slots" mount): images by canonical name, list order
	// = swap order; when either list is nonempty the romExt path never runs
	std::vector<std::string> floppyImages;
	std::vector<std::string> cdImages;
	bool hddMounted = false; // a HardDiskDrive.img memory file exists
	std::string bootDrive = "none"; // "a"/"c": boot that drive instead of the DOS shell
	std::string extraConf;   // appended last (a .conf rom's text)
};

// The composed dosbox-x.conf: base conf + machine preset + the settings'
// sections + an autoexec mounting the loaded file ("rom") and the hard disk.
std::string dosdrv_compose_conf(const DosDrvMachine &m);

// The embedded pre-formatted FAT16 disks ("21mb".."2014mb"): returns the
// full image size and points at the zstd-compressed head, or 0 for none.
uint64_t dosdrv_formatted_disk(const std::string &name, const uint8_t **zst, size_t *zstLen);

struct DosDrvJoystick {
	bool up = false, down = false, left = false, right = false;
	bool button1 = false, button2 = false;
};

struct DosDrvMouse {
	int32_t posX = 0, posY = 0;      // absolute, in the 800x600 driver range
	int32_t speedX = 0, speedY = 0;  // relative movement this frame
	bool leftPressed = false, middlePressed = false, rightPressed = false;
	bool leftReleased = false, middleReleased = false, rightReleased = false;
	float sensitivity = 3.0f; // BizHawk's Mouse Relative Sensitivity default
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

// Turbo: while off, the finished surface is not copied out for the frontend.
// Only that - see the comment on the definition for why DOSBox-X's own
// disablerender switch cannot be used here.
void dosdrv_set_rendering(bool on);

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
/* Whether that kind of drive was read or written during the frame just run.
   One per medium, because a machine with a disc AND a hard disk has two lights
   and they mean different things. */
bool dosdrv_cd_activity();
bool dosdrv_disk_activity();
// Nonzero while dosbox reads input this frame - the lag-frame signal.
bool dosdrv_input_was_read();

// Memory domains, in a fixed order: 0 = Conventional Memory, 1 = Upper Memory
// Area, 2 = Extended Memory (may be absent), 3 = Physical RAM, 4 = Video RAM,
// 5 = Hard Disk Drive (absent when no HDD is mounted).
// Returns false when index is past the end.
bool dosdrv_domain(int index, const char **name, uint8_t **data, uint64_t *size, bool *writable);

// The writable hard disk image (the savedata export): NULL/0 when none.
// The writable hard disk. There is no buffer to hand over: the base is read
// from the host and only written chunks are in guest memory (sparse-disk.h),
// so the image is READ rather than pointed at - which is also what the
// save-data export does to produce a whole mountable .hdd.
uint64_t dosdrv_hdd_size();
bool dosdrv_hdd_read(uint64_t offset, void *dst, size_t len);
