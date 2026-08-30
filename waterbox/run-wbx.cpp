// Standalone driver for the waterboxed DOSBox-X core: runs core.wbx through
// the miniBox host EXACTLY as the frontend does - the file mounted as "rom" +
// "rom.name", the machine knobs as the "settings" JSON the guest composes its
// configuration from, and input through the SetButton wide-input export - and
// reports the same digests as run-native, so the two builds diff directly.
//
// usage: run-wbx <core.wbx> [--rom FILE] [--preset NAME] [--formatted-hdd N]
//        [--memsize MB] [--cycles N] [--joysticks] [--frames N] [--type TEXT]
//        [--rerecord] [--turbo] [--savedata-out DIR]
//
// The typing schedule stays in lockstep with run-native's: one key event
// phase per frame from frame 140, 2 held + 2 released per character.
#include "minibox.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "exercise-input.h"
#include <keyboard.h> // KBD_* values; no other dosbox headers needed

static bool writeTga(const char *path, const uint32_t *bgra, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	uint8_t hdr[18] = {0};
	hdr[2] = 2;
	hdr[12] = w & 0xff; hdr[13] = (w >> 8) & 0xff;
	hdr[14] = h & 0xff; hdr[15] = (h >> 8) & 0xff;
	hdr[16] = 32;
	hdr[17] = 0x20;
	fwrite(hdr, 1, 18, f);
	fwrite(bgra, 4, (size_t)w * h, f);
	fclose(f);
	return true;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	if (!h) h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}

typedef struct { FILE *f; } freader;
static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s) { return (intptr_t)fread(d, 1, s, ((freader *)ud)->f); }
typedef struct { const uint8_t *p; size_t n, pos; } memreader;
static intptr_t mem_reader(uintptr_t ud, uint8_t *d, uintptr_t s)
{
	memreader *m = (memreader *)ud;
	size_t take = s < (m->n - m->pos) ? s : (m->n - m->pos);
	memcpy(d, m->p + m->pos, take); m->pos += take; return (intptr_t)take;
}
typedef struct { uint8_t *b; size_t len, cap, pos; } membuf;
static int32_t mem_write(uintptr_t ud, const uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	if (m->len + n > m->cap) { m->cap = (m->len + n) * 2 + 64; m->b = (uint8_t *)realloc(m->b, m->cap); }
	memcpy(m->b + m->len, d, n); m->len += n; return 0;
}
static intptr_t mem_read(uintptr_t ud, uint8_t *d, uintptr_t n)
{
	membuf *m = (membuf *)ud;
	uintptr_t avail = m->len - m->pos; if (n > avail) n = avail;
	memcpy(d, m->b + m->pos, n); m->pos += n; return (intptr_t)n;
}

// MUST match run-native.cpp's keyForChar exactly - the gate compares replays.
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

typedef int (MB_GUEST_ABI *intfn)(void);
typedef void (MB_GUEST_ABI *framefn)(uint64_t);
typedef void (MB_GUEST_ABI *setfn)(int32_t, int32_t);
typedef void (MB_GUEST_ABI *voidfn_i)(int);
typedef uintptr_t (MB_GUEST_ABI *ptrfn)(void);
typedef uintptr_t (MB_GUEST_ABI *ptrfn_i)(int);
typedef int64_t (MB_GUEST_ABI *i64fn_i)(int);

static uintptr_t proc(mb_host *h, const char *n)
{
	mb_return r; wbx_get_proc_addr(h, n, &r);
	if (r.error_message[0]) { fprintf(stderr, "proc %s: %s\n", n, r.error_message); exit(2); }
	if (!r.data) { fprintf(stderr, "missing required export %s\n", n); exit(2); }
	return r.data;
}

static void makeParentDirs(const char *path)
{
	char dir[1024];
	for (size_t i = 1; path[i] && i + 1 < sizeof dir; i++) {
		if (path[i] != '/') continue;
		memcpy(dir, path, i); dir[i] = 0;
		mkdir(dir, 0777);
	}
}

static int exportSaveData(mb_host *h, const char *dir)
{
	intfn Count = (intfn)proc(h, "GetSaveDataFileCount");
	ptrfn_i Name = (ptrfn_i)proc(h, "GetSaveDataFileName");
	i64fn_i Size = (i64fn_i)proc(h, "GetSaveDataFileSize");
	// STREAMED, the same way the engine reads it. The hard disk has no
	// contiguous buffer to point at - its base is on the host and only written
	// chunks are in guest memory - so it is served a window at a time and
	// reassembled here. Exactly what comes out of Chimera's Export Save Data.
	typedef int64_t (MB_GUEST_ABI *readfn)(int32_t, int64_t, int64_t);
	readfn Read = (readfn)proc(h, "ReadSaveDataFile");
	ptrfn Scratch = (ptrfn)proc(h, "GetSaveDataScratch");
	int n = Count();
	for (int i = 0; i < n; i++) {
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", dir, (const char *)Name(i));
		makeParentDirs(path);
		FILE *f = fopen(path, "wb");
		if (!f) { fprintf(stderr, "could not write %s\n", path); return 0; }
		int64_t size = Size(i), done = 0;
		int ok = 1;
		while (done < size && ok) {
			int64_t got = Read(i, done, size - done);
			if (got <= 0) { ok = 0; break; }
			ok = fwrite((const void *)Scratch(), 1, (size_t)got, f) == (size_t)got;
			done += got;
		}
		fclose(f);
		if (!ok || done != size) {
			fprintf(stderr, "could not write %s (%lld of %lld bytes)\n",
				path, (long long)done, (long long)size);
			return 0;
		}
	}
	printf("savedata=%d\n", n);
	return 1;
}

int main(int argc, char **argv)
{
	const char *wbxPath = nullptr, *rom = nullptr;
	const char *typeText = nullptr, *savedataOut = nullptr;
	const char *dumpPrefix = nullptr;
	const char *preset = nullptr, *formattedHdd = nullptr, *bootDrive = nullptr;
	std::vector<std::string> extraFiles; // NAME=PATH, mounted as NAME
	std::vector<std::pair<long, int>> swapCd; // FRAME:INDEX schedules
	std::vector<std::pair<long, int>> swapFd;
	int memsize = -1000000, cycles = -1000000; // sentinel: not given
	long frames = 600;
	bool rerecord = false, turbo = false, joysticks = false, exercise = false;
	long turboSettle = 0;
	std::vector<std::string> extraSettings; // KEY=VALUE, string or number

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
		else if (!strcmp(argv[i], "--preset") && i + 1 < argc) preset = argv[++i];
		else if (!strcmp(argv[i], "--formatted-hdd") && i + 1 < argc) formattedHdd = argv[++i];
		else if (!strcmp(argv[i], "--boot-drive") && i + 1 < argc) bootDrive = argv[++i];
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
		else if (!strcmp(argv[i], "--memsize") && i + 1 < argc) memsize = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtol(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--type") && i + 1 < argc) typeText = argv[++i];
		else if (!strcmp(argv[i], "--savedata-out") && i + 1 < argc) savedataOut = argv[++i];
		else if (!strcmp(argv[i], "--dump-video") && i + 1 < argc) dumpPrefix = argv[++i];
		else if (!strcmp(argv[i], "--rerecord")) rerecord = true;
		else if (!strcmp(argv[i], "--turbo")) turbo = true;
		else if (!strcmp(argv[i], "--turbo-settle") && i + 1 < argc) turboSettle = strtol(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--exercise")) exercise = true;
		else if (!strcmp(argv[i], "--joysticks")) joysticks = true;
		else if (!strcmp(argv[i], "--setting") && i + 1 < argc) extraSettings.push_back(argv[++i]);
		else if (!wbxPath) wbxPath = argv[i];
		else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
	}
	if (!wbxPath) {
		fprintf(stderr, "usage: run-wbx <core.wbx> [--rom FILE] [--preset NAME] ...\n");
		return 2;
	}

	FILE *wf = fopen(wbxPath, "rb");
	if (!wf) { fprintf(stderr, "cannot open %s\n", wbxPath); return 1; }

	// THE PACKAGE'S LAYOUT, copied. waterbox.config's memoryLayoutMiB is what
	// Chimera gives the guest; this runner builds its own host and so has to
	// say the same thing, or the gate proves a machine nobody runs. The mmap
	// arena is the one that matters here: the whole writable hard disk lives in
	// it, and the largest this package offers is 2014mb.
	mb_memory_layout_template layout = { 256u << 20, 16u << 20, 16u << 20, 64u << 20, 3456ull << 20 };
	freader fr = { wf };
	mb_return r;
	wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(wf);
	if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); return 1; }
	mb_host *h = (mb_host *)r.data;

	// the file, under the frontend's fixed names
	if (rom) {
		// BY PATH, as Chimera does. wbx_mount_file reads the whole file into
		// host memory first; a hard disk image is the one file here most likely
		// to be enormous, and the machine reads it a sector at a time anyway.
		wbx_mount_file_path(h, "rom", rom, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount rom: %s\n", r.error_message); return 1; }
		const char *base = strrchr(rom, '/');
		base = base ? base + 1 : rom;
		memreader nr = { (const uint8_t *)base, strlen(base), 0 };
		wbx_mount_file(h, "rom.name", mem_reader, (uintptr_t)&nr, false, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount rom.name: %s\n", r.error_message); return 1; }
	}

	for (const std::string &spec : extraFiles) {
		auto eq = spec.find('=');
		if (eq == std::string::npos) { fprintf(stderr, "--extra-file wants NAME=PATH\n"); return 2; }
		std::string name = spec.substr(0, eq), path = spec.substr(eq + 1);
		FILE *f = fopen(path.c_str(), "rb");
		if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
		freader rd = { f };
		wbx_mount_file(h, name.c_str(), file_read, (uintptr_t)&rd, false, &r);
		fclose(f);
		if (r.error_message[0]) { fprintf(stderr, "mount %s: %s\n", name.c_str(), r.error_message); return 1; }
	}

	// the settings channel, exactly the frontend's shape (a flat JSON object)
	std::string settings = "{";
	auto addStr = [&](const char *k, const char *v) {
		if (settings.size() > 1) settings += ",";
		settings += std::string("\"") + k + "\":\"" + v + "\"";
	};
	auto addNum = [&](const char *k, long v) {
		if (settings.size() > 1) settings += ",";
		settings += std::string("\"") + k + "\":" + std::to_string(v);
	};
	if (preset) addStr("machinePreset", preset);
	if (formattedHdd) addStr("formattedHardDisk", formattedHdd);
	if (bootDrive) addStr("bootDrive", bootDrive);
	if (memsize != -1000000) addNum("memsizeMB", memsize);
	if (cycles != -1000000) addNum("cpuCycles", cycles);
	if (joysticks) {
		if (settings.size() > 1) settings += ",";
		settings += "\"joystick1Enabled\":true,\"joystick2Enabled\":true";
	}
	for (const std::string &spec : extraSettings) {
		auto eq = spec.find('=');
		if (eq == std::string::npos) { fprintf(stderr, "--setting wants KEY=VALUE\n"); return 2; }
		std::string k = spec.substr(0, eq), v = spec.substr(eq + 1);
		char *end = nullptr;
		strtod(v.c_str(), &end);
		bool numeric = end && *end == '\0' && !v.empty();
		bool boolean = v == "true" || v == "false";
		if (settings.size() > 1) settings += ",";
		if (numeric || boolean) settings += "\"" + k + "\":" + v;
		else settings += "\"" + k + "\":\"" + v + "\"";
	}
	settings += "}";
	{
		memreader sr = { (const uint8_t *)settings.data(), settings.size(), 0 };
		wbx_mount_file(h, "settings", mem_reader, (uintptr_t)&sr, false, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount settings: %s\n", r.error_message); return 1; }
	}

	wbx_activate_host(h, &r);
	intfn Init = (intfn)proc(h, "Init");
	if (Init() != 1) {
		ptrfn GetLoadError = (ptrfn)proc(h, "GetLoadError");
		fprintf(stderr, "Init failed: %s\n", (const char *)GetLoadError());
		return 1;
	}

	framefn FrameAdvance = (framefn)proc(h, "FrameAdvance");
	voidfn_i SetRenderingEnabled = (voidfn_i)proc(h, "SetRenderingEnabled");
	setfn SetButton = (setfn)proc(h, "SetButton");
	setfn SetAxis = (setfn)proc(h, "SetAxis");
	ptrfn GetVideoBgra = (ptrfn)proc(h, "GetVideoBgra");
	intfn GetVideoWidth = (intfn)proc(h, "GetVideoWidth");
	intfn GetVideoHeight = (intfn)proc(h, "GetVideoHeight");
	ptrfn GetAudio = (ptrfn)proc(h, "GetAudio");
	intfn GetAudioSampleCount = (intfn)proc(h, "GetAudioSampleCount");
	intfn GetMemoryDomainCount = (intfn)proc(h, "GetMemoryDomainCount");
	ptrfn_i GetMemoryDomainName = (ptrfn_i)proc(h, "GetMemoryDomainName");
	ptrfn_i GetMemoryDomainPtr = (ptrfn_i)proc(h, "GetMemoryDomainPtr");
	i64fn_i GetMemoryDomainSize = (i64fn_i)proc(h, "GetMemoryDomainSize");

	wbx_deactivate_host(h, &r);
	wbx_seal(h, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 1; }
	wbx_activate_host(h, &r);

	uint64_t vh = 0, ah = 0;
	/* the second half of the run, hashed separately: see --turbo */
	const long tail = frames / 2;
	const long hashFrom = tail + turboSettle;
	uint64_t th = 0;
	membuf st = {0};
	size_t typePos = 0;
	int typePhase = 0;
	int prevKey = -1, prevShift = 0;
	int pendingCdShadow = 0, pendingFdShadow = 0; // mirror the guest's selections
	bool swapHeld = false;

	for (long i = 0; i < frames; i++) {
		/* turbo: draw nothing for the first half of the run, then draw the
		 * second half normally - those are the pictures the turbo leg
		 * compares */
		if (turbo) SetRenderingEnabled(i >= tail);
		if (rerecord) {
			st.len = 0;
			wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
			st.pos = 0;
			wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "rerecord: %s\n", r.error_message); return 1; }
		}

		// typing through the WIDE-INPUT export, in lockstep with run-native:
		// config button index = EX_BTN_KEYS + KBD value - 1 (gen-config.py)
		int key = -1, shift = 0;
		if (typeText && i >= 140 && typeText[typePos] != '\0') {
			bool sh = false;
			KBD_KEYS k = keyForChar(typeText[typePos], &sh);
			if (typePhase < 2) {
				if (k != KBD_NONE) key = EX_BTN_KEYS + (int)k - 1;
				if (sh) shift = 1;
			}
			if (++typePhase == 4) { typePhase = 0; typePos++; }
		}
		if (key != prevKey) {
			if (prevKey >= 0) SetButton(prevKey, 0);
			if (key >= 0) SetButton(key, 1);
			prevKey = key;
		}
		if (shift != prevShift) {
			SetButton(EX_BTN_KEYS + (int)KBD_leftshift - 1, shift);
			prevShift = shift;
		}

		// scheduled disk swaps, through the SWAP BUTTONS exactly as a
		// player would press them: a selector step and the swap button
		// rising together (the guest steps the pending index before
		// applying the swap, so one frame moves one image). The frame in
		// the schedule is when the driver receives the change, matching
		// run-native's direct path. Button block: +0..2 floppy
		// prev/next/swap, +3..5 CD prev/next/swap.
		if (swapHeld) {
			for (int b = 0; b < 6; b++) SetButton(EX_BTN_SWAP + b, 0);
			swapHeld = false;
		}
		auto pressSwap = [&](const std::vector<std::pair<long, int>> &sched, int base, int &shadow) {
			for (const auto &sc : sched) {
				if (sc.first != i) continue;
				int steps = sc.second - shadow;
				if (steps > 1 || steps < -1) { fprintf(stderr, "swap steps > 1 not supported\n"); exit(2); }
				if (steps > 0) SetButton(base + 1, 1);
				if (steps < 0) SetButton(base + 0, 1);
				SetButton(base + 2, 1);
				shadow = sc.second;
				swapHeld = true;
			}
		};
		pressSwap(swapFd, EX_BTN_SWAP + 0, pendingFdShadow);
		pressSwap(swapCd, EX_BTN_SWAP + 3, pendingCdShadow);
		if (exercise) {
			// the shared pattern, driven exactly as the frontend drives the
			// guest: axes through SetAxis, button LEVELS through SetButton
			// (the adapter converts mouse levels to edges itself)
			ExLevels ex = exercise_levels(i);
			SetAxis(0, ex.posX);
			SetAxis(1, ex.posY);
			SetAxis(2, ex.spdX);
			SetAxis(3, ex.spdY);
			SetButton(EX_BTN_MOUSE + 0, ex.mouseL);
			SetButton(EX_BTN_MOUSE + 2, ex.mouseR);
			SetButton(EX_BTN_JOY1 + 0, ex.joyUp);
			SetButton(EX_BTN_JOY1 + 1, ex.joyDown);
			SetButton(EX_BTN_JOY1 + 2, ex.joyLeft);
			SetButton(EX_BTN_JOY1 + 3, ex.joyRight);
			SetButton(EX_BTN_JOY1 + 4, ex.joyB1);
			SetButton(EX_BTN_JOY1 + 5, ex.joyB2);
		}
		FrameAdvance(0);

		int w = GetVideoWidth(), hgt = GetVideoHeight();
		const void *video = (const void *)GetVideoBgra();
		int nsamp = GetAudioSampleCount();
		const void *audio = (const void *)GetAudio();
		if (video) vh = fnv(vh, video, (size_t)w * hgt * 4);
		if (video && i >= hashFrom) {
			th = fnv(th, &w, sizeof w);
			th = fnv(th, &hgt, sizeof hgt);
			th = fnv(th, video, (size_t)w * hgt * 4);
		}
		ah = fnv(ah, audio, (size_t)nsamp * 4);
		if (dumpPrefix && video) {
			char path[1024];
			snprintf(path, sizeof path, "%s%05ld.tga", dumpPrefix, i);
			writeTga(path, (const uint32_t *)video, w, hgt);
		}
	}

	printf("frames=%ld\n", frames);
	printf("videoHash=%016llx\n", (unsigned long long)vh);
	printf("tailVideoHash=%016llx\n", (unsigned long long)th);
	printf("audioHash=%016llx\n", (unsigned long long)ah);
	int nd = GetMemoryDomainCount();
	for (int i = 0; i < nd; i++) {
		const char *dname = (const char *)GetMemoryDomainName(i);
		if (!dname) continue;
		uint64_t dh = fnv(0, (const void *)GetMemoryDomainPtr(i), (size_t)GetMemoryDomainSize(i));
		printf("domain[%s]=%016llx\n", dname, (unsigned long long)dh);
	}

	if (savedataOut && !exportSaveData(h, savedataOut)) {
		wbx_deactivate_host(h, &r); wbx_destroy_host(h, &r);
		return 1;
	}

	wbx_deactivate_host(h, &r); wbx_destroy_host(h, &r);
	free(st.b);
	return 0;
}
