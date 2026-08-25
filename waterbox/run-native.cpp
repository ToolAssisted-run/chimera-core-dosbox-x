// Host driver over dosbox-driver: the native reference machine. The
// configuration is COMPOSED BY THE DRIVER from the same machine knobs the
// guest reads from its settings channel, so the two builds run byte-identical
// configuration by construction.
//
// usage: run-native [--workdir DIR] [--preset NAME] [--formatted-hdd NAME]
//                   [--memsize MB] [--cycles N] [--joysticks]
//                   [--rom FILE] [--extra-conf FILE] [--autoexec LINE]...
//                   [--frames N] [--gate] [--verbose]
//                   [--dump-video PREFIX] [--type TEXT] [--savedata-out DIR]
//
// --rom stages the file as "rom" (the frontend's fixed mount name) and routes
// it by extension exactly as the guest does: .hdd seeds the writable disk,
// .conf appends configuration, floppy/disc extensions become imgmount lines.
// --type feeds TEXT as keystrokes once the machine has booted.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <ctime>
#include <sys/time.h>

#include "dosbox-driver.h"
#include "exercise-input.h"
#include <keyboard.h>

// The guest's clock is frozen by miniBox (a constant 2017-05-27 12:44:28 UTC,
// the sandbox epoch); freeze the native build the same way so both machines
// boot from the identical instant. Strong definitions here shadow libc's.
static const time_t kSandboxEpoch = 1495889068;
extern "C" time_t time(time_t *t) { if (t) *t = kSandboxEpoch; return kSandboxEpoch; }
extern "C" int gettimeofday(struct timeval *tv, void *) { if (tv) { tv->tv_sec = kSandboxEpoch; tv->tv_usec = 0; } return 0; }
extern "C" int clock_gettime(clockid_t, struct timespec *ts) { if (ts) { ts->tv_sec = kSandboxEpoch; ts->tv_nsec = 0; } return 0; }

static uint64_t fnv1a(const void *data, size_t len, uint64_t h = 1469598103934665603ULL)
{
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ULL; }
	return h;
}

static bool readWholeFile(const char *path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	uint8_t chunk[1 << 16];
	size_t got;
	out.clear();
	while ((got = fread(chunk, 1, sizeof chunk, f)) != 0) out.insert(out.end(), chunk, chunk + got);
	bool ok = !ferror(f);
	fclose(f);
	return ok;
}

static bool writeWholeFile(const std::string &path, const void *data, size_t len)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	bool ok = len == 0 || fwrite(data, 1, len, f) == len;
	fclose(f);
	return ok;
}

static bool writeTga(const char *path, const uint32_t *bgra, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	uint8_t hdr[18] = {0};
	hdr[2] = 2;
	hdr[12] = w & 0xff; hdr[13] = (w >> 8) & 0xff;
	hdr[14] = h & 0xff; hdr[15] = (h >> 8) & 0xff;
	hdr[16] = 32;
	hdr[17] = 0x20; // top-left origin
	fwrite(hdr, 1, 18, f);
	fwrite(bgra, 4, (size_t)w * h, f);
	fclose(f);
	return true;
}

// ASCII -> KBD_KEYS (the subset --type needs; unshifted only, plus a few)
static KBD_KEYS keyForChar(char c, bool *shift)
{
	*shift = false;
	if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
	else if (c >= 'A' && c <= 'Z') *shift = true;
	switch (c) {
		case 'A': return KBD_a; case 'B': return KBD_b; case 'C': return KBD_c;
		case 'D': return KBD_d; case 'E': return KBD_e; case 'F': return KBD_f;
		case 'G': return KBD_g; case 'H': return KBD_h; case 'I': return KBD_i;
		case 'J': return KBD_j; case 'K': return KBD_k; case 'L': return KBD_l;
		case 'M': return KBD_m; case 'N': return KBD_n; case 'O': return KBD_o;
		case 'P': return KBD_p; case 'Q': return KBD_q; case 'R': return KBD_r;
		case 'S': return KBD_s; case 'T': return KBD_t; case 'U': return KBD_u;
		case 'V': return KBD_v; case 'W': return KBD_w; case 'X': return KBD_x;
		case 'Y': return KBD_y; case 'Z': return KBD_z;
		case '0': return KBD_0; case '1': return KBD_1; case '2': return KBD_2;
		case '3': return KBD_3; case '4': return KBD_4; case '5': return KBD_5;
		case '6': return KBD_6; case '7': return KBD_7; case '8': return KBD_8;
		case '9': return KBD_9;
		case ' ': return KBD_space;
		case '.': return KBD_period;
		case '-': return KBD_minus;
		case '=': return KBD_equals;
		case '>': *shift = true; return KBD_period;
		case ':': *shift = true; return KBD_semicolon;
		case '\\': return KBD_backslash;
		case '\n': return KBD_enter;
		default: return KBD_NONE;
	}
}

int main(int argc, char **argv)
{
	// determinism: the frozen clock renders in one time zone everywhere
	setenv("TZ", "UTC", 1);
	tzset();

	std::vector<std::string> autoexec;
	const char *rom = nullptr;
	const char *extraConfFile = nullptr;
	std::vector<std::string> extraFiles; // NAME=PATH, staged beside the rom
	std::vector<std::pair<long, int>> swapCd; // FRAME:INDEX schedules
	std::vector<std::pair<long, int>> swapFd;
	const char *workdir = "work-native";
	const char *dumpPrefix = nullptr;
	const char *typeText = nullptr;
	const char *savedataOut = nullptr;
	const char *sliceOut = nullptr;
	unsigned long sliceOff = 0, sliceLen = 0;
	int frames = 600;
	bool gate = false, verbose = false, exercise = false;
	const char *formattedHdd = "none";
	DosDrvMachine m;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--preset") && i + 1 < argc) m.machinePreset = argv[++i];
		else if (!strcmp(argv[i], "--formatted-hdd") && i + 1 < argc) formattedHdd = argv[++i];
		else if (!strcmp(argv[i], "--memsize") && i + 1 < argc) m.memsizeMB = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) m.cpuCycles = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
		else if (!strcmp(argv[i], "--extra-conf") && i + 1 < argc) extraConfFile = argv[++i];
		else if (!strcmp(argv[i], "--extra-file") && i + 1 < argc) extraFiles.push_back(argv[++i]);
		else if (!strcmp(argv[i], "--swap-cd") && i + 1 < argc) {
			long fr = 0; int idx = 0;
			if (sscanf(argv[++i], "%ld:%d", &fr, &idx) != 2) { fprintf(stderr, "--swap-cd wants FRAME:INDEX\n"); return 2; }
			swapCd.push_back({ fr, idx });
		}
		else if (!strcmp(argv[i], "--swap-fd") && i + 1 < argc) {
			long fr = 0; int idx = 0;
			if (sscanf(argv[++i], "%ld:%d", &fr, &idx) != 2) { fprintf(stderr, "--swap-fd wants FRAME:INDEX\n"); return 2; }
			swapFd.push_back({ fr, idx });
		}
		else if (!strcmp(argv[i], "--autoexec") && i + 1 < argc) autoexec.push_back(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--workdir") && i + 1 < argc) workdir = argv[++i];
		else if (!strcmp(argv[i], "--dump-video") && i + 1 < argc) dumpPrefix = argv[++i];
		else if (!strcmp(argv[i], "--type") && i + 1 < argc) typeText = argv[++i];
		else if (!strcmp(argv[i], "--savedata-out") && i + 1 < argc) savedataOut = argv[++i];
		else if (!strcmp(argv[i], "--ram-slice") && i + 3 < argc) {
			sliceOff = strtoul(argv[++i], 0, 0);
			sliceLen = strtoul(argv[++i], 0, 0);
			sliceOut = argv[++i];
		}
		else if (!strcmp(argv[i], "--gate")) gate = true;
		else if (!strcmp(argv[i], "--exercise")) exercise = true;
		else if (!strcmp(argv[i], "--verbose")) verbose = true;
		else if (!strcmp(argv[i], "--joysticks")) m.joysticks = true;
		else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
	}

	// ---- stage the work directory and compose the machine -----------------
	// The loaded file lands as "rom" (the frontend's fixed mount name); the
	// configuration comes from dosdrv_compose_conf - the same function the
	// guest runs - and reaches dosbox through the driver's in-memory conf.
	mkdir(workdir, 0777);

	DosDrvConfig cfg;
	cfg.joystick1Enabled = m.joysticks;
	cfg.joystick2Enabled = m.joysticks;

	std::string romExt;
	if (rom) {
		const char *dot = strrchr(rom, '.');
		if (dot) {
			romExt = dot;
			for (char &c : romExt) c = (char)tolower((unsigned char)c);
		}
		std::vector<uint8_t> bytes;
		if (!readWholeFile(rom, bytes)) { fprintf(stderr, "cannot read %s\n", rom); return 1; }
		if (!writeWholeFile(std::string(workdir) + "/rom", bytes.data(), bytes.size())) return 1;
		if (romExt == ".hdd") {
			cfg.hddSeedFile = "rom";
			cfg.writableHDDImageSize = (bytes.size() + 511) / 512 * 512;
			m.hddMounted = true;
		} else if (romExt == ".conf") {
			m.extraConf.assign((const char *)bytes.data(), bytes.size());
		} else {
			m.romExt = romExt;
		}
	}
	for (const std::string &spec : extraFiles) {
		// a cue sheet's referenced track file, extra discs, any named sibling
		auto eq = spec.find('=');
		if (eq == std::string::npos) { fprintf(stderr, "--extra-file wants NAME=PATH\n"); return 2; }
		std::vector<uint8_t> bytes;
		if (!readWholeFile(spec.substr(eq + 1).c_str(), bytes)) { fprintf(stderr, "cannot read %s\n", spec.c_str()); return 1; }
		if (!writeWholeFile(std::string(workdir) + "/" + spec.substr(0, eq), bytes.data(), bytes.size())) return 1;
	}
	// extra swappable images staged as rom2..romN, same probe the guest runs
	for (int i = 2; i <= 8; i++) {
		struct stat st;
		std::string name = std::string(workdir) + "/rom" + std::to_string(i);
		if (stat(name.c_str(), &st) != 0) break;
		m.extraImageCount++;
	}
	if (!m.hddMounted && strcmp(formattedHdd, "none") != 0) {
		const uint8_t *zst = nullptr;
		size_t zstLen = 0;
		uint64_t size = dosdrv_formatted_disk(formattedHdd, &zst, &zstLen);
		if (size == 0) { fprintf(stderr, "unknown formatted disk '%s'\n", formattedHdd); return 2; }
		cfg.hddSeedZst = zst;
		cfg.hddSeedZstLen = zstLen;
		cfg.writableHDDImageSize = size;
		m.hddMounted = true;
	}
	if (extraConfFile) {
		std::vector<uint8_t> bytes;
		if (!readWholeFile(extraConfFile, bytes)) { fprintf(stderr, "cannot read %s\n", extraConfFile); return 1; }
		m.extraConf.append((const char *)bytes.data(), bytes.size());
	}
	for (const std::string &line : autoexec) {
		m.extraConf += "\n[autoexec]\n" + line + "\n";
	}
	cfg.confText = dosdrv_compose_conf(m);
	if (getenv("DOSDRV_PRINT_CONF")) fputs(cfg.confText.c_str(), stderr);
	if (chdir(workdir) != 0) { fprintf(stderr, "cannot enter %s\n", workdir); return 1; }

	// ---- boot -------------------------------------------------------------
	std::string err;
	if (!dosdrv_boot(cfg, &err)) {
		fprintf(stderr, "boot failed: %s\n", err.c_str());
		return 1;
	}

	// ---- frames -----------------------------------------------------------
	uint64_t gateV = 0, gateA = 0;
	size_t typePos = 0;
	int typePhase = 0; // interleave press / release frames
	ExLevels prevEx = exercise_levels(0);
	for (int i = 0; i < frames; i++) {
		DosDrvInput in;
		// the frame slice follows the machine's reported refresh, exactly as
		// the guest adapter does - the digests must line up frame for frame
		{
			int rn = 0, rd = 0;
			dosdrv_refresh_rate(&rn, &rd);
			if (rn > 0 && rd > 0) { in.framerateNumerator = rn; in.framerateDenominator = rd; }
		}
		// --type: one keystroke every 4 frames, starting after 2 seconds
		if (typeText && i >= 140 && typeText[typePos] != '\0') {
			bool shift = false;
			KBD_KEYS k = keyForChar(typeText[typePos], &shift);
			if (typePhase < 2) { // held for 2 frames, released for 2
				if (k != KBD_NONE) in.keys[k] = 1;
				if (shift) in.keys[KBD_leftshift] = 1;
			}
			if (++typePhase == 4) { typePhase = 0; typePos++; }
		}
		for (const auto &sc : swapCd) {
			if (sc.first == i) in.insertCDROM = sc.second;
		}
		for (const auto &sf : swapFd) {
			if (sf.first == i) in.insertFloppyDisk = sf.second;
		}
		if (exercise) {
			// the shared deterministic pattern; levels become the driver's
			// edges here, exactly as the guest adapter converts them
			ExLevels ex = exercise_levels(i);
			in.mouse.posX = ex.posX;
			in.mouse.posY = ex.posY;
			in.mouse.speedX = ex.spdX;
			in.mouse.speedY = ex.spdY;
			in.mouse.leftPressed = ex.mouseL && !prevEx.mouseL;
			in.mouse.leftReleased = !ex.mouseL && prevEx.mouseL;
			in.mouse.rightPressed = ex.mouseR && !prevEx.mouseR;
			in.mouse.rightReleased = !ex.mouseR && prevEx.mouseR;
			in.joy1.up = ex.joyUp != 0;
			in.joy1.down = ex.joyDown != 0;
			in.joy1.left = ex.joyLeft != 0;
			in.joy1.right = ex.joyRight != 0;
			in.joy1.button1 = ex.joyB1 != 0;
			in.joy1.button2 = ex.joyB2 != 0;
			prevEx = ex;
		}
		dosdrv_frame(in);

		int w = 0, h = 0, nsamp = 0;
		const uint32_t *video = dosdrv_video(&w, &h);
		const int16_t *audio = dosdrv_audio(&nsamp);
		if (gate) {
			if (video) gateV = fnv1a(video, (size_t)w * h * 4, gateV ? gateV : 1469598103934665603ULL);
			gateA = fnv1a(audio, (size_t)nsamp * 4, gateA ? gateA : 1469598103934665603ULL);
		} else if (verbose) {
			printf("frame=%d w=%d h=%d video=%016llx audio=%016llx samples=%d ticks=%u\n",
				i, w, h,
				(unsigned long long)(video ? fnv1a(video, (size_t)w * h * 4) : 0),
				(unsigned long long)fnv1a(audio, (size_t)nsamp * 4), nsamp,
				dosdrv_last_frame_ticks());
		}
		if (dumpPrefix && video) {
			char path[1024];
			snprintf(path, sizeof path, "%s%05d.tga", dumpPrefix, i);
			writeTga(path, video, w, h);
		}
	}

	if (gate) {
		printf("frames=%d\n", frames);
		printf("videoHash=%016llx\n", (unsigned long long)gateV);
		printf("audioHash=%016llx\n", (unsigned long long)gateA);
		for (int i = 0;; i++) {
			const char *dn; uint8_t *dd; uint64_t ds; bool dw;
			if (!dosdrv_domain(i, &dn, &dd, &ds, &dw)) break;
			printf("domain[%s]=%016llx\n", dn, (unsigned long long)fnv1a(dd, ds));
		}
	}
	if (sliceOut) {
		// domain 0 is Conventional Memory - the frontend witness compares this
		const char *dn; uint8_t *dd; uint64_t ds; bool dw;
		if (dosdrv_domain(0, &dn, &dd, &ds, &dw) && sliceOff + sliceLen <= ds) {
			if (!writeWholeFile(sliceOut, dd + sliceOff, sliceLen)) return 1;
		} else {
			fprintf(stderr, "ram slice out of range\n");
			return 1;
		}
	}

	if (savedataOut) {
		// the savedata export, flattened for the gate to diff: one file,
		// the whole writable disk image
		if (dosdrv_hdd_size() != 0) {
			mkdir(savedataOut, 0777);
			std::string path = std::string(savedataOut) + "/HardDiskDrive.img";
			if (!writeWholeFile(path, dosdrv_hdd_buffer(), (size_t)dosdrv_hdd_size())) {
				fprintf(stderr, "could not write %s\n", path.c_str());
				return 1;
			}
			printf("savedata=1\n");
		} else {
			printf("savedata=0\n");
		}
	}

	int rn = 0, rd = 0;
	dosdrv_refresh_rate(&rn, &rd);
	printf("refresh=%d/%d ticks=%u\n", rn, rd, dosdrv_ticks_elapsed());
	return 0;
}
