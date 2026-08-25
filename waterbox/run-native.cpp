// Host driver over dosbox-driver: composes a dosbox-x.conf in a work
// directory, boots the machine, runs frames with scripted/blank input, and
// reports per-frame video/audio digests (the gate format).
//
// usage: run-native [--workdir DIR] [--conf FILE]... [--autoexec LINE]...
//                   [--hdd IMG] [--hdd-grow BYTES] [--floppy IMG]...
//                   [--frames N] [--gate] [--verbose] [--joysticks]
//                   [--dump-video PREFIX] [--type TEXT]
//
// --type feeds TEXT as keystrokes (one key per frame, DOS scancodes derived
// from ASCII) once the machine has booted - enough to run COMMAND.COM lines.
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

	std::vector<const char *> confs;
	std::vector<std::string> autoexec;
	std::vector<const char *> floppies;
	const char *hdd = nullptr;
	const char *workdir = "work-native";
	const char *dumpPrefix = nullptr;
	const char *typeText = nullptr;
	uint64_t hddGrow = 0;
	int frames = 600;
	bool gate = false, verbose = false, joysticks = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--conf") && i + 1 < argc) confs.push_back(argv[++i]);
		else if (!strcmp(argv[i], "--autoexec") && i + 1 < argc) autoexec.push_back(argv[++i]);
		else if (!strcmp(argv[i], "--hdd") && i + 1 < argc) hdd = argv[++i];
		else if (!strcmp(argv[i], "--hdd-grow") && i + 1 < argc) hddGrow = strtoull(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--floppy") && i + 1 < argc) floppies.push_back(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--workdir") && i + 1 < argc) workdir = argv[++i];
		else if (!strcmp(argv[i], "--dump-video") && i + 1 < argc) dumpPrefix = argv[++i];
		else if (!strcmp(argv[i], "--type") && i + 1 < argc) typeText = argv[++i];
		else if (!strcmp(argv[i], "--gate")) gate = true;
		else if (!strcmp(argv[i], "--verbose")) verbose = true;
		else if (!strcmp(argv[i], "--joysticks")) joysticks = true;
		else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
	}

	// ---- compose the work directory: conf + input files, then chdir -------
	// (in the guest these are mounted files; natively they are real ones, and
	// dosbox-x reads dosbox-x.conf from its working directory)
	mkdir(workdir, 0777);
	std::string conf;
	for (const char *c : confs) {
		std::vector<uint8_t> bytes;
		if (!readWholeFile(c, bytes)) { fprintf(stderr, "cannot read %s\n", c); return 1; }
		conf.append((const char *)bytes.data(), bytes.size());
		conf += "\n";
	}
	conf += "[joystick]\njoysticktype = ";
	conf += joysticks ? "2axis\n" : "none\n";
	conf += "\n[autoexec]\n@echo off\n";

	auto stage = [&](const char *src, const std::string &name) {
		std::vector<uint8_t> bytes;
		if (!readWholeFile(src, bytes)) { fprintf(stderr, "cannot read %s\n", src); return false; }
		return writeWholeFile(std::string(workdir) + "/" + name, bytes.data(), bytes.size());
	};

	if (!floppies.empty()) {
		std::string line = "imgmount a ";
		for (size_t i = 0; i < floppies.size(); i++) {
			const char *ext = strrchr(floppies[i], '.');
			std::string name = "FloppyDisk" + std::to_string(i) + (ext ? ext : ".img");
			if (!stage(floppies[i], name)) return 1;
			line += name + " ";
		}
		conf += line + "\n";
	}
	if (hdd) {
		if (!stage(hdd, "HardDiskDrive")) return 1;
		conf += "imgmount c HardDiskDrive.img\n";
	}
	for (const std::string &line : autoexec) conf += line + "\n";

	if (!writeWholeFile(std::string(workdir) + "/dosbox-x.conf", conf.data(), conf.size())) return 1;
	if (chdir(workdir) != 0) { fprintf(stderr, "cannot enter %s\n", workdir); return 1; }

	// ---- boot -------------------------------------------------------------
	DosDrvConfig cfg;
	cfg.joystick1Enabled = joysticks;
	cfg.joystick2Enabled = joysticks;
	if (hdd) {
		struct stat st;
		if (stat("HardDiskDrive", &st) != 0) return 1;
		cfg.writableHDDImageSize = hddGrow > (uint64_t)st.st_size ? hddGrow : (uint64_t)st.st_size;
	}
	std::string err;
	if (!dosdrv_boot(cfg, &err)) {
		fprintf(stderr, "boot failed: %s\n", err.c_str());
		return 1;
	}

	// ---- frames -----------------------------------------------------------
	uint64_t gateV = 0, gateA = 0;
	size_t typePos = 0;
	int typePhase = 0; // interleave press / release frames
	for (int i = 0; i < frames; i++) {
		DosDrvInput in;
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
	int rn = 0, rd = 0;
	dosdrv_refresh_rate(&rn, &rd);
	printf("refresh=%d/%d ticks=%u\n", rn, rd, dosdrv_ticks_elapsed());
	return 0;
}
