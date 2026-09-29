// oracle-run: scripted native driver for decompilation work. Same machine composition as
// run-native, plus a frame script (keys held per frame, RAM dumps, tracer probes/watches,
// instruction logs) and an event dump. Output paths are used as given (absolute recommended).
//
// usage: oracle-run --workdir DIR --rom FILE [--setting NAME=VALUE]... [--autoexec LINE]...
//                   --script FILE --events FILE [--dump-video PREFIX] [--verbose]
// script lines (frames are 0-based; '#' comments):
//   key F NAME 0|1          set key level from frame F on (NAME = KBD enum name without KBD_, e.g. leftshift, esc, up)
//   ram F PATH              write the 640 KB conventional memory after frame F
//   probe SEG OFF [label [PHYS LEN]]   record registers+stack whenever CS:IP == SEG:OFF (hex); PHYS/LEN (hex) add a memory sample
//                           (PHYS "SSxxxx" = ss:sp+xxxx, "DSxxxx" = ds:xxxx)
//   probe32 EIP label [PHYS LEN]   like probe, matching the full 32-bit EIP in any code segment (flat protected mode)
//   watch PHYS LEN [label]  record every change of LEN bytes at physical PHYS (hex)
//   trace F1 F2 PATH        log every instruction executed during frames F1..F2 to PATH (cs:ip + opcode bytes)
//   inject F whenCS whenIP cs ip ax bx cx dx si di ds es   call cs:ip (hex) with those registers when CS:IP==whenCS:whenIP at/after frame F
//   poke F PHYS HEXBYTES    write bytes into conventional memory after frame F
//   shot F PATH             write the screen after frame F as a 24-bit TGA
//   mem F DOMAIN PATH       write a whole memory domain (index as dosdrv_domain; 0 = conventional) after frame F
//   audio F1 F2 PATH        write the mixer output of frames F1..F2 to PATH (raw s16 stereo interleaved, 44100 Hz);
//                           PATH.frames gets "frame sample_pairs instructions_at_frame_end" per frame
//   mouse F X Y             absolute mouse position (driver range 0..800 x 0..600) from frame F on; a change is sent as movement
//   mrel F DX DY            relative mouse movement (mickey speed) on frame F only
//   button F left|right|middle 0|1   press (1) or release (0) a mouse button on frame F
//   end F                   stop after frame F
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#include <ctime>
#include <sys/time.h>
#include "dosbox-driver.h"
#include "tracer.h"
#include <keyboard.h>

static const time_t kSandboxEpoch = 1495889068;
extern "C" time_t time(time_t *t) { if (t) *t = kSandboxEpoch; return kSandboxEpoch; }
extern "C" int gettimeofday(struct timeval *tv, void *) { if (tv) { tv->tv_sec = kSandboxEpoch; tv->tv_usec = 0; } return 0; }
extern "C" int clock_gettime(clockid_t, struct timespec *ts) { if (ts) { ts->tv_sec = kSandboxEpoch; ts->tv_nsec = 0; } return 0; }

static bool readWholeFile(const char *path, std::vector<uint8_t> &out) {
	FILE *f = fopen(path, "rb"); if (!f) return false;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	out.resize(n > 0 ? (size_t)n : 0); bool ok = n <= 0 || fread(out.data(), 1, out.size(), f) == out.size();
	fclose(f); return ok;
}
static bool writeWholeFile(const std::string &path, const void *data, size_t len) {
	FILE *f = fopen(path.c_str(), "wb"); if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
	bool ok = fwrite(data, 1, len, f) == len; fclose(f); return ok;
}
static bool writeTga(const char *path, const uint32_t *bgra, int w, int h) {
	FILE *f = fopen(path, "wb"); if (!f) return false;
	uint8_t hdr[18] = {0}; hdr[2] = 2; hdr[12] = w & 255; hdr[13] = w >> 8; hdr[14] = h & 255; hdr[15] = h >> 8; hdr[16] = 24; hdr[17] = 0x20;
	fwrite(hdr, 1, 18, f);
	for (int i = 0; i < w * h; i++) { uint8_t p[3] = {(uint8_t)bgra[i], (uint8_t)(bgra[i] >> 8), (uint8_t)(bgra[i] >> 16)}; fwrite(p, 1, 3, f); }
	fclose(f); return true;
}

// KBD enum names, in enum order (keyboard.h)
static const char *kbdNames[] = {
	"none","1","2","3","4","5","6","7","8","9","0","q","w","e","r","t","y","u","i","o","p","a","s","d","f","g","h","j","k","l","z","x","c","v","b","n","m",
	"f1","f2","f3","f4","f5","f6","f7","f8","f9","f10","f11","f12",
	"esc","tab","backspace","enter","space","leftalt","rightalt","leftctrl","rightctrl","leftshift","rightshift",
	"capslock","scrolllock","numlock","grave","minus","equals","backslash","leftbracket","rightbracket","semicolon","quote","period","comma","slash","extra_lt_gt",
	"printscreen","pause","insert","home","pageup","delete","end","pagedown","left","up","down","right",
	"kp1","kp2","kp3","kp4","kp5","kp6","kp7","kp8","kp9","kp0","kpdivide","kpmultiply","kpminus","kpplus","kpenter","kpperiod",
};
static int keyIndex(const std::string &n) {
	for (size_t i = 0; i < sizeof kbdNames / sizeof *kbdNames; i++) if (n == kbdNames[i]) return (int)i;
	return -1;
}

struct KeyEv { int frame, key, level; };
struct MouseEv { int frame, kind, a, b; };   // kind 0 = absolute pos, 1 = relative, 2 = button (a = 0 left 1 right 2 middle, b = level)
struct RamDump { int frame; std::string path; };
struct TraceReq { int f1, f2; std::string path; };
struct InjectReq { int frame; uint16_t whenCs, whenIp; TracerRegs regs; bool done; };
struct PokeReq { int frame; uint32_t phys; std::vector<uint8_t> bytes; };

int main(int argc, char **argv) {
	const char *workdir = "oracle-work", *rom = nullptr, *scriptPath = nullptr, *eventsPath = nullptr, *dumpPrefix = nullptr;
	bool verbose = false;
	std::vector<std::string> autoexec;
	DosDrvMachine m; m.joystick1 = m.joystick2 = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--setting") && i + 1 < argc) {
			std::string s = argv[++i]; auto eq = s.find('=');
			if (eq == std::string::npos || !dosdrv_machine_setting(m, s.substr(0, eq), s.substr(eq + 1))) { fprintf(stderr, "bad --setting %s\n", s.c_str()); return 2; }
		}
		else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
		else if (!strcmp(argv[i], "--autoexec") && i + 1 < argc) autoexec.push_back(argv[++i]);
		else if (!strcmp(argv[i], "--workdir") && i + 1 < argc) workdir = argv[++i];
		else if (!strcmp(argv[i], "--script") && i + 1 < argc) scriptPath = argv[++i];
		else if (!strcmp(argv[i], "--events") && i + 1 < argc) eventsPath = argv[++i];
		else if (!strcmp(argv[i], "--dump-video") && i + 1 < argc) dumpPrefix = argv[++i];
		else if (!strcmp(argv[i], "--verbose")) verbose = true;
		else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
	}
	if (!rom || !scriptPath) { fprintf(stderr, "need --rom and --script\n"); return 2; }

	// ---- script ----
	struct ShotReq { int frame; std::string path; int domain; }; std::vector<ShotReq> shots, mems;
	struct AudioReq { int f1, f2; std::string path; FILE *f, *idx; }; std::vector<AudioReq> audios;
	struct ProbePokeReq { std::string label; uint32_t hit, phys; std::vector<uint8_t> bytes; }; std::vector<ProbePokeReq> probePokes;
	struct Probe32Req { uint32_t eip, phys, len; }; std::vector<Probe32Req> probes32; std::vector<std::string> probe32Labels;
	std::vector<KeyEv> keys; std::vector<MouseEv> mouseEvs; std::vector<RamDump> rams; std::vector<TraceReq> traces; std::vector<InjectReq> injects; std::vector<PokeReq> pokes; std::vector<std::string> probeLabels, watchLabels;
	std::vector<std::pair<uint16_t,uint16_t>> probes; std::vector<std::pair<uint32_t,uint32_t>> probeMem; std::vector<std::pair<uint32_t,uint32_t>> watches;
	int endFrame = 600;
	{
		FILE *f = fopen(scriptPath, "r"); if (!f) { fprintf(stderr, "cannot read %s\n", scriptPath); return 1; }
		char line[1024];
		while (fgets(line, sizeof line, f)) {
			char *h = strchr(line, '#'); if (h) *h = 0;
			char a[64] = {0}, b[512] = {0}, c[512] = {0}, d[512] = {0}, e5[512] = {0}, e6[64] = {0}; int n = sscanf(line, "%63s %511s %511s %511s %511s %63s", a, b, c, d, e5, e6);
			if (n < 1) continue;
			std::string cmd = a;
			if (cmd == "key" && n >= 4) { int k = keyIndex(c); if (k < 0) { fprintf(stderr, "unknown key %s\n", c); return 2; } keys.push_back({atoi(b), k, atoi(d)}); }
			else if (cmd == "ram" && n >= 3) rams.push_back({atoi(b), c});
			else if (cmd == "probe" && n >= 3) { probes.push_back({(uint16_t)strtoul(b, 0, 16), (uint16_t)strtoul(c, 0, 16)}); probeLabels.push_back(n >= 4 ? d : ""); probeMem.push_back(n >= 6 ? std::make_pair(!strncmp(e5, "SS", 2) ? 0xFFFF0000u | (uint32_t)strtoul(e5 + 2, 0, 16) : !strncmp(e5, "DS", 2) ? 0xFFFE0000u | (uint32_t)strtoul(e5 + 2, 0, 16) : (uint32_t)strtoul(e5, 0, 16), (uint32_t)strtoul(e6, 0, 16)) : std::make_pair(0u, 0u)); }
			else if (cmd == "probe32" && n >= 3) { probes32.push_back({(uint32_t)strtoul(b, 0, 16), n >= 5 ? (uint32_t)strtoul(d, 0, 16) : 0u, n >= 5 ? (uint32_t)strtoul(e5, 0, 16) : 0u}); probe32Labels.push_back(c); }
			else if (cmd == "watch" && n >= 3) { watches.push_back({(uint32_t)strtoul(b, 0, 16), (uint32_t)strtoul(c, 0, 16)}); watchLabels.push_back(n >= 4 ? d : ""); }
			else if (cmd == "trace" && n >= 4) traces.push_back({atoi(b), atoi(c), d});
			else if (cmd == "inject") {
				unsigned v[13]; int fr;
				if (sscanf(line, "inject %d %x %x %x %x %x %x %x %x %x %x %x %x", &fr, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) != 13) { fprintf(stderr, "bad inject line: %s", line); return 2; }
				InjectReq r{}; r.frame = fr; r.whenCs = (uint16_t)v[0]; r.whenIp = (uint16_t)v[1];
				r.regs.cs = (uint16_t)v[2]; r.regs.eip = v[3]; r.regs.eax = v[4]; r.regs.ebx = v[5]; r.regs.ecx = v[6]; r.regs.edx = v[7]; r.regs.esi = v[8]; r.regs.edi = v[9]; r.regs.ds = (uint16_t)v[10]; r.regs.es = (uint16_t)v[11]; r.regs.ss = 0xFFFF;
				injects.push_back(r);
			}
			else if (cmd == "poke" && n >= 4) { PokeReq r; r.frame = atoi(b); r.phys = (uint32_t)strtoul(c, 0, 16); for (size_t i = 0; i + 1 < strlen(d); i += 2) { unsigned x; sscanf(d + i, "%2x", &x); r.bytes.push_back((uint8_t)x); } pokes.push_back(r); }
			else if (cmd == "probepoke" && n >= 5) {   // probepoke LABEL HIT PHYS HEX: write at the HIT-th hit of the probe labelled LABEL
				ProbePokeReq r; r.label = b; r.hit = (uint32_t)atoi(c); r.phys = (uint32_t)strtoul(d, 0, 16);
				for (size_t i = 0; i + 1 < strlen(e5); i += 2) { unsigned x; sscanf(e5 + i, "%2x", &x); r.bytes.push_back((uint8_t)x); }
				probePokes.push_back(r);
			}
			else if (cmd == "shot" && n >= 3) shots.push_back({atoi(b), c, 0});
			else if (cmd == "mem" && n >= 4) mems.push_back({atoi(b), d, atoi(c)});
			else if (cmd == "audio" && n >= 4) audios.push_back({atoi(b), atoi(c), d, nullptr, nullptr});
			else if (cmd == "mouse" && n >= 4) mouseEvs.push_back({atoi(b), 0, atoi(c), atoi(d)});
			else if (cmd == "mrel" && n >= 4) mouseEvs.push_back({atoi(b), 1, atoi(c), atoi(d)});
			else if (cmd == "button" && n >= 4) mouseEvs.push_back({atoi(b), 2, !strcmp(c, "left") ? 0 : !strcmp(c, "right") ? 1 : 2, atoi(d)});
			else if (cmd == "end" && n >= 2) endFrame = atoi(b);
			else { fprintf(stderr, "bad script line: %s", line); return 2; }
		}
		fclose(f);
	}

	// ---- stage machine (as run-native) ----
	mkdir(workdir, 0777);
	DosDrvConfig cfg; cfg.joystick1Enabled = false; cfg.joystick2Enabled = false;
	{
		std::string romExt; const char *dot = strrchr(rom, '.');
		if (dot) { romExt = dot; for (char &ch : romExt) ch = (char)tolower((unsigned char)ch); }
		std::vector<uint8_t> bytes;
		if (!readWholeFile(rom, bytes)) { fprintf(stderr, "cannot read %s\n", rom); return 1; }
		if (!writeWholeFile(std::string(workdir) + "/rom", bytes.data(), bytes.size())) return 1;
		if (romExt == ".hdd" || romExt == ".hdi") { cfg.hddSeedFile = "rom"; cfg.writableHDDImageSize = (bytes.size() + 511) / 512 * 512; m.hddMounted = true; cfg.hddIsHdi = m.hddIsHdi = romExt == ".hdi"; }
		else if (romExt == ".conf") m.extraConf.assign((const char *)bytes.data(), bytes.size());
		else m.romExt = romExt;
	}
	for (const std::string &line : autoexec) m.extraConf += "\n[autoexec]\n" + line + "\n";
	cfg.confText = dosdrv_compose_conf(m);
	if (chdir(workdir) != 0) { fprintf(stderr, "cannot enter %s\n", workdir); return 1; }
	std::string err;
	if (!dosdrv_boot(cfg, &err)) { fprintf(stderr, "boot failed: %s\n", err.c_str()); return 1; }

	// ---- arm tracer ----
	for (size_t i = 0; i < probes.size(); i++) { if (probeMem[i].second) tracer_add_probe_mem(probes[i].first, probes[i].second, probeMem[i].first, probeMem[i].second); else tracer_add_probe(probes[i].first, probes[i].second); }
	for (const auto &pp : probePokes) {
		int id = -1; for (size_t i = 0; i < probeLabels.size(); i++) if (probeLabels[i] == pp.label) { id = (int)i; break; }
		if (id < 0) { fprintf(stderr, "probepoke: no probe labelled %s\n", pp.label.c_str()); return 2; }
		tracer_probe_poke(id, pp.hit, pp.phys, pp.bytes.data(), (uint32_t)pp.bytes.size());
	}
	for (size_t i = 0; i < probes32.size(); i++) { tracer_add_probe32(probes32[i].eip, probes32[i].phys, probes32[i].len); probeLabels.push_back(probe32Labels[i]); }
	for (auto &w : watches) tracer_add_watch(w.first, w.second);
	FILE *ev = eventsPath ? fopen(eventsPath, "w") : nullptr;
	if (eventsPath && !ev) { fprintf(stderr, "cannot write %s\n", eventsPath); return 1; }

	DosDrvInput in;
	FILE *traceFile = nullptr;
	for (int fr = 0; fr <= endFrame; fr++) {
		for (auto &k : keys) if (k.frame == fr) in.keys[k.key] = (uint8_t)k.level;
		in.mouse.speedX = in.mouse.speedY = 0;
		in.mouse.leftPressed = in.mouse.rightPressed = in.mouse.middlePressed = false;
		in.mouse.leftReleased = in.mouse.rightReleased = in.mouse.middleReleased = false;
		for (auto &m : mouseEvs) if (m.frame == fr) {
			if (m.kind == 0) { in.mouse.posX = m.a; in.mouse.posY = m.b; }
			else if (m.kind == 1) { in.mouse.speedX = m.a; in.mouse.speedY = m.b; }
			else {
				bool *p = m.b ? (m.a == 0 ? &in.mouse.leftPressed : m.a == 1 ? &in.mouse.rightPressed : &in.mouse.middlePressed)
				              : (m.a == 0 ? &in.mouse.leftReleased : m.a == 1 ? &in.mouse.rightReleased : &in.mouse.middleReleased);
				*p = true;
			}
		}
		{ int rn = 0, rd = 0; dosdrv_refresh_rate(&rn, &rd); if (rn > 0 && rd > 0) { in.framerateNumerator = rn; in.framerateDenominator = rd; } }
		bool traceOn = false;
		for (auto &t : traces) if (fr >= t.f1 && fr <= t.f2) { traceOn = true; if (!traceFile) traceFile = fopen(t.path.c_str(), "w"); }
		tracer_log_instructions(traceOn);
		for (auto &r : injects) if (!r.done && r.frame <= fr && tracer_inject_state() != 1 && tracer_inject_state() != 2) { r.done = true; tracer_inject(r.whenCs, r.whenIp, r.regs); }
		dosdrv_frame(in);
		for (auto &a : audios) if (fr >= a.f1 && fr <= a.f2) {
			if (!a.f && (!(a.f = fopen(a.path.c_str(), "wb")) || !(a.idx = fopen((a.path + ".frames").c_str(), "w")))) { fprintf(stderr, "cannot write %s\n", a.path.c_str()); return 1; }
			int np = 0; const int16_t *pcm = dosdrv_audio(&np);
			if (pcm && np > 0) fwrite(pcm, 4, (size_t)np, a.f);
			fprintf(a.idx, "%d %d %llu\n", fr, pcm ? np : 0, (unsigned long long)tracer_instr_count());
			if (fr == a.f2) { fclose(a.f); fclose(a.idx); a.f = a.idx = nullptr; }
		}
		for (auto &r : pokes) if (r.frame == fr) tracer_poke(r.phys, r.bytes.data(), (uint32_t)r.bytes.size());
		// drain events
		uint32_t n = tracer_event_count();
		for (uint32_t i = 0; i < n; i++) {
			const TracerEvent *e = tracer_event(i);
			if (e->kind == TRACER_INSTR) {
				if (traceFile) { fprintf(traceFile, "%d %llu %04X:%04X", fr, (unsigned long long)e->instrCount, e->cs, e->ip); for (int j = 0; j < 8; j++) fprintf(traceFile, "%s%02X", j ? "" : " ", e->opcode[j]); fprintf(traceFile, " ax=%04X sp=%04X ds=%04X\n", e->regs.eax & 0xFFFF, e->regs.esp & 0xFFFF, e->regs.ds); }
			} else if (ev && e->kind == TRACER_PROBE) {
				const TracerRegs &r = e->regs;
				fprintf(ev, "frame=%d n=%llu probe=%u %s at %04X:%04X ax=%04X bx=%04X cx=%04X dx=%04X si=%04X di=%04X bp=%04X sp=%04X ds=%04X es=%04X ss=%04X fl=%04X stack=",
					fr, (unsigned long long)e->instrCount, e->id, probeLabels[e->id].c_str(), e->cs, e->ip, r.eax & 0xFFFF, r.ebx & 0xFFFF, r.ecx & 0xFFFF, r.edx & 0xFFFF, r.esi & 0xFFFF, r.edi & 0xFFFF, r.ebp & 0xFFFF, r.esp & 0xFFFF, r.ds, r.es, r.ss, r.eflags & 0xFFFF);
				for (int j = 0; j < 32; j++) fprintf(ev, "%02X", e->stack[j]);
				fprintf(ev, " dsdx=");
				for (int j = 0; j < 32; j++) fprintf(ev, "%02X", e->dsdx[j]);
				fprintf(ev, " mem=");
				{ uint32_t ml = 0; const uint8_t *m = tracer_event_mem(e, &ml); if (ml < 64) { m = e->mem; ml = 64; } for (uint32_t j = 0; j < ml; j++) fprintf(ev, "%02X", m[j]); }
				fprintf(ev, "\n");
			} else if (ev && (e->kind == TRACER_INJECT_START || e->kind == TRACER_INJECT_DONE)) {
				const TracerRegs &r = e->regs;
				fprintf(ev, "frame=%d n=%llu %s cs:ip=%04X:%04X ax=%04X bx=%04X cx=%04X dx=%04X ds=%04X es=%04X ss:sp=%04X:%04X fl=%04X\n", fr, (unsigned long long)e->instrCount, e->kind == TRACER_INJECT_START ? "inject-start" : "inject-done", r.cs, r.eip & 0xFFFF, r.eax & 0xFFFF, r.ebx & 0xFFFF, r.ecx & 0xFFFF, r.edx & 0xFFFF, r.ds, r.es, r.ss, r.esp & 0xFFFF, r.eflags & 0xFFFF);
			} else if (ev && e->kind == TRACER_WATCH) {
				fprintf(ev, "frame=%d n=%llu watch=%u %s at %04X:%04X addr=%05X %X -> %X\n", fr, (unsigned long long)e->instrCount, e->id, watchLabels[e->id].c_str(), e->cs, e->ip, e->addr, e->oldVal, e->newVal);
			}
		}
		if (tracer_events_dropped()) fprintf(stderr, "frame %d: %u tracer events dropped (ring full)\n", fr, tracer_events_dropped());
		tracer_events_clear();
		if (!traceOn && traceFile) { fclose(traceFile); traceFile = nullptr; }
		int w = 0, h = 0; const uint32_t *video = dosdrv_video(&w, &h);
		if (dumpPrefix && video) { char path[1024]; snprintf(path, sizeof path, "%s%05d.tga", dumpPrefix, fr); writeTga(path, video, w, h); }
		for (auto &s2 : shots) if (s2.frame == fr && video) writeTga(s2.path.c_str(), video, w, h);
		for (auto &m2 : mems) if (m2.frame == fr) { const char *dn; uint8_t *dd; uint64_t ds; bool dw; if (dosdrv_domain(m2.domain, &dn, &dd, &ds, &dw)) { writeWholeFile(m2.path, dd, (size_t)ds); fprintf(stderr, "domain %d %s: %llu bytes\n", m2.domain, dn, (unsigned long long)ds); } }
		for (auto &r : rams) if (r.frame == fr) {
			const char *dn; uint8_t *dd; uint64_t ds; bool dw;
			if (dosdrv_domain(0, &dn, &dd, &ds, &dw)) writeWholeFile(r.path, dd, (size_t)ds);
		}
		if (verbose && fr % 100 == 0) { fprintf(stderr, "frame %d instr=%llu ticks=%u\n", fr, (unsigned long long)tracer_instr_count(), dosdrv_ticks_elapsed()); }
	}
	if (traceFile) fclose(traceFile);
	if (ev) fclose(ev);
	for (auto &a : audios) if (a.f) { fclose(a.f); fclose(a.idx); }
	printf("done frames=%d instructions=%llu\n", endFrame + 1, (unsigned long long)tracer_instr_count());
	return 0;
}
