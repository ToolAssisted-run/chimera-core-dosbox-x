// Standalone driver for the waterboxed DOSBox-X core: runs core.wbx through
// the miniBox host and reports the same per-frame video/audio and final
// memory-domain digests as run-native, so the two builds diff directly.
//
// usage: run-wbx <core.wbx> --conf <composed dosbox-x.conf> [--frames N]
//        [--hdd IMG] [--hdd-grow BYTES] [--floppy IMG]... [--type TEXT]
//        [--joysticks] [--rerecord] [--savedata-out DIR]
//
// The conf arrives PRE-COMPOSED (the gate takes the one run-native wrote), so
// both machines read byte-identical configuration. The typing schedule must
// stay in lockstep with run-native's: one key event phase every frame from
// frame 140, 2 held + 2 released per character.
#include "minibox.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "waterbox-input.h"
#include <keyboard.h> // KBD_* values; no other dosbox headers needed

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
		case '>': *shift = true; return KBD_period;
		case ':': *shift = true; return KBD_semicolon;
		case '\\': return KBD_backslash;
		case '\n': return KBD_enter;
		default: return KBD_NONE;
	}
}

typedef int (MB_GUEST_ABI *intfn)(void);
typedef void (MB_GUEST_ABI *framefn)(uint64_t);
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

// --savedata-out: walk the savedata guest ABI group exactly as the frontend
// does and write the tree for the gate to diff against run-native's.
static int exportSaveData(mb_host *h, const char *dir)
{
	intfn Count = (intfn)proc(h, "GetSaveDataFileCount");
	ptrfn_i Name = (ptrfn_i)proc(h, "GetSaveDataFileName");
	i64fn_i Size = (i64fn_i)proc(h, "GetSaveDataFileSize");
	ptrfn_i Buffer = (ptrfn_i)proc(h, "GetSaveDataFileBuffer");
	int n = Count();
	for (int i = 0; i < n; i++) {
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", dir, (const char *)Name(i));
		makeParentDirs(path);
		FILE *f = fopen(path, "wb");
		if (!f) { fprintf(stderr, "could not write %s\n", path); return 0; }
		int64_t size = Size(i);
		int ok = size == 0 || fwrite((const void *)Buffer(i), 1, (size_t)size, f) == (size_t)size;
		fclose(f);
		if (!ok) { fprintf(stderr, "could not write %s\n", path); return 0; }
	}
	printf("savedata=%d\n", n);
	return 1;
}

int main(int argc, char **argv)
{
	const char *wbxPath = nullptr, *confPath = nullptr, *hdd = nullptr;
	const char *typeText = nullptr, *savedataOut = nullptr;
	std::vector<const char *> floppies;
	uint64_t hddGrow = 0;
	long frames = 600;
	bool rerecord = false, joysticks = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--conf") && i + 1 < argc) confPath = argv[++i];
		else if (!strcmp(argv[i], "--hdd") && i + 1 < argc) hdd = argv[++i];
		else if (!strcmp(argv[i], "--hdd-grow") && i + 1 < argc) hddGrow = strtoull(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--floppy") && i + 1 < argc) floppies.push_back(argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtol(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--type") && i + 1 < argc) typeText = argv[++i];
		else if (!strcmp(argv[i], "--savedata-out") && i + 1 < argc) savedataOut = argv[++i];
		else if (!strcmp(argv[i], "--rerecord")) rerecord = true;
		else if (!strcmp(argv[i], "--joysticks")) joysticks = true;
		else if (!wbxPath) wbxPath = argv[i];
		else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
	}
	if (!wbxPath || !confPath) {
		fprintf(stderr, "usage: run-wbx <core.wbx> --conf <dosbox-x.conf> [--frames N] ...\n");
		return 2;
	}

	FILE *wf = fopen(wbxPath, "rb");
	if (!wf) { fprintf(stderr, "cannot open %s\n", wbxPath); return 1; }

	// DOSBox is a big machine: its RAM (up to 64MB+), SDL, the mixer and the
	// HDD memory file all live in ordinary heaps; big allocations (the HDD)
	// go to musl's mmap path.
	mb_memory_layout_template layout = { 256u << 20, 16u << 20, 16u << 20, 64u << 20, 1024u << 20 };
	freader fr = { wf };
	mb_return r;
	wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(wf);
	if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); return 1; }
	mb_host *h = (mb_host *)r.data;

	auto mountFile = [&](const char *name, const char *path) {
		FILE *f = fopen(path, "rb");
		if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
		freader rd = { f };
		mb_return rr;
		wbx_mount_file(h, name, file_read, (uintptr_t)&rd, false, &rr);
		fclose(f);
		if (rr.error_message[0]) { fprintf(stderr, "mount %s: %s\n", name, rr.error_message); exit(1); }
	};

	mountFile("dosbox-x.conf", confPath);

	uint64_t hddSize = 0;
	if (hdd) {
		struct stat st;
		if (stat(hdd, &st) != 0) { fprintf(stderr, "cannot stat %s\n", hdd); return 1; }
		hddSize = hddGrow > (uint64_t)st.st_size ? hddGrow : (uint64_t)st.st_size;
		mountFile("HardDiskDrive", hdd);
	}
	for (size_t i = 0; i < floppies.size(); i++) {
		const char *ext = strrchr(floppies[i], '.');
		std::string name = "FloppyDisk" + std::to_string(i) + (ext ? ext : ".img");
		mountFile(name.c_str(), floppies[i]);
	}

	// the boot channel waterbox.cpp reads (hdd size + joystick enables)
	char drvconfig[128];
	snprintf(drvconfig, sizeof drvconfig, "hddSize=%llu\njoy1=%d\njoy2=%d\n",
		(unsigned long long)hddSize, joysticks ? 1 : 0, joysticks ? 1 : 0);
	{
		memreader mr = { (const uint8_t *)drvconfig, strlen(drvconfig), 0 };
		wbx_mount_file(h, "drvconfig", mem_reader, (uintptr_t)&mr, false, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount drvconfig: %s\n", r.error_message); return 1; }
	}

	wbx_activate_host(h, &r);
	intfn Init = (intfn)proc(h, "Init");
	if (Init() != 1) {
		ptrfn GetLoadError = (ptrfn)proc(h, "GetLoadError");
		fprintf(stderr, "Init failed: %s\n", (const char *)GetLoadError());
		return 1;
	}

	framefn FrameAdvance = (framefn)proc(h, "FrameAdvance");
	ptrfn GetInputBuffer = (ptrfn)proc(h, "GetInputBuffer");
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

	WbxInput *input = (WbxInput *)GetInputBuffer();
	uint64_t vh = 0, ah = 0;
	membuf st = {0};
	size_t typePos = 0;
	int typePhase = 0;

	for (long i = 0; i < frames; i++) {
		if (rerecord) {
			st.len = 0;
			wbx_save_state(h, mem_write, (uintptr_t)&st, &r);
			st.pos = 0;
			wbx_load_state(h, mem_read, (uintptr_t)&st, &r);
			if (r.error_message[0]) { fprintf(stderr, "rerecord: %s\n", r.error_message); return 1; }
		}

		memset(input->keys, 0, sizeof input->keys);
		input->insertFloppyDisk = -1;
		input->insertCDROM = -1;
		// the typing schedule, in lockstep with run-native
		if (typeText && i >= 140 && typeText[typePos] != '\0') {
			bool shift = false;
			KBD_KEYS k = keyForChar(typeText[typePos], &shift);
			if (typePhase < 2) {
				if (k != KBD_NONE) input->keys[k] = 1;
				if (shift) input->keys[KBD_leftshift] = 1;
			}
			if (++typePhase == 4) { typePhase = 0; typePos++; }
		}

		FrameAdvance(0);

		int w = GetVideoWidth(), hgt = GetVideoHeight();
		const void *video = (const void *)GetVideoBgra();
		int nsamp = GetAudioSampleCount();
		const void *audio = (const void *)GetAudio();
		if (video) vh = fnv(vh, video, (size_t)w * hgt * 4);
		ah = fnv(ah, audio, (size_t)nsamp * 4);
	}

	printf("frames=%ld\n", frames);
	printf("videoHash=%016llx\n", (unsigned long long)vh);
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
