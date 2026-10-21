#include "tracer.h"
#include <vector>
#include <algorithm>
#include <cstring>
#include <string>
#include <cstdio>
#include "dosbox.h"
#include "cpu.h"
#include "regs.h"
#include "mem.h"
#include "../src/cpu/lazyflags.h"

static int injState = 0; static uint16_t injWhenCs, injWhenIp; static TracerRegs injRegs, injSaved;

bool tracer_active = false;
static bool logInstr = false;
static uint64_t instrCount = 0;
static uint32_t cap = 1u << 20;
static std::vector<TracerEvent> ring;
static uint32_t head = 0, count = 0, dropped = 0;
static std::vector<uint8_t> arena;   // large probe samples, cleared with the events

struct Probe { uint16_t cs, ip; uint32_t phys, len; bool eip32 = false; uint32_t eip = 0; };   // eip32: match the full EIP, any CS (flat protected mode)
struct Watch { uint32_t phys, len, last; };
static std::vector<Probe> probes;
static std::vector<uint32_t> probeHits;
struct ProbePoke { int probe; uint32_t hit, phys; std::vector<uint8_t> bytes; };
static std::vector<ProbePoke> probePokes;   // sorted by (probe, hit) as added; applied when the hit comes
static std::vector<Watch> watches;

static FILE *rlog = nullptr; static bool rlogPipe = false; static uint16_t progLo = 0, progHi = 0; static uint64_t progCount = 0;
static bool inProg = false, repRestart = false, irqOutside = false; static uint16_t repCs = 0, repIp = 0, irqCs = 0, irqIp = 0;
// the nesting of excursions: 'E' an excursion (the program called out), 'H' a program handler of an interrupt taken
// while outside (it returns into the excursion at cs:ip; its entry and that return are not excursion ends)
struct Level { char kind; uint16_t cs, ip; };
static std::vector<Level> levels;
static void recompute() { tracer_active = logInstr || !probes.empty() || !watches.empty() || injState == 1 || injState == 2 || rlog; }

// port reads, run-length coded: the same port, width, value and instruction again and again (a polling loop)
static struct { unsigned port, width; uint32_t value; uint16_t cs, ip; uint64_t pos, last; uint32_t n; } pr; static bool prPending = false;
static void pr_flush() {
	if (!prPending) return;
	fprintf(rlog, "P %llu %X %u %X %04X:%04X %u %llu\n", (unsigned long long)pr.pos, pr.port, pr.width, pr.value, pr.cs, pr.ip, pr.n, (unsigned long long)pr.last);
	prPending = false;
}
static void log_regs(char kind) {
	TracerRegs r; tracer_read_regs(&r);
	fprintf(rlog, "%c %llu %04X:%04X ax=%04X bx=%04X cx=%04X dx=%04X si=%04X di=%04X bp=%04X sp=%04X ds=%04X es=%04X ss=%04X fl=%04X\n", kind, (unsigned long long)progCount,
		r.cs, r.eip & 0xFFFF, r.eax & 0xFFFF, r.ebx & 0xFFFF, r.ecx & 0xFFFF, r.edx & 0xFFFF, r.esi & 0xFFFF, r.edi & 0xFFFF, r.ebp & 0xFFFF, r.esp & 0xFFFF, r.ds, r.es, r.ss, r.eflags & 0xFFFF);
}
// memory dumps at the program's instruction counts (a replay's): the 640 KB as the program finds it after POS of its
// instructions (after the excursion of the last of them, before an interrupt taken there): the first of the
// program's next instruction or an interrupt taken in the program at that count
static std::vector<std::pair<uint64_t, std::string>> ramAt; static size_t ramAtNext = 0;
void tracer_replay_ram_at(uint64_t pos, const char *path) {
	ramAt.push_back({pos, path});
	std::stable_sort(ramAt.begin(), ramAt.end(), [](const std::pair<uint64_t, std::string> &a, const std::pair<uint64_t, std::string> &b) { return a.first < b.first; });
}
static void ram_at_check() {
	while (ramAtNext < ramAt.size() && ramAt[ramAtNext].first <= progCount) {
		if (ramAt[ramAtNext].first == progCount) {
			FILE *f = fopen(ramAt[ramAtNext].second.c_str(), "wb");
			if (f) { for (uint32_t a = 0; a < 0xA0000; a++) fputc(phys_readb(a), f); fclose(f); }
		}
		ramAtNext++;
	}
}
// a path ending in .zst is written through zstd
void tracer_replay_open(const char *path, uint16_t lo, uint16_t hi) {
	size_t n = strlen(path);
	rlogPipe = n > 4 && !strcmp(path + n - 4, ".zst");
	if (rlogPipe) { std::string cmd = std::string("zstd -q -3 -T4 -f -o '") + path + "'"; rlog = popen(cmd.c_str(), "w"); }
	else rlog = fopen(path, "w");
	progLo = lo; progHi = hi; progCount = 0; inProg = repRestart = prPending = irqOutside = false; levels.clear();
	if (rlog) setvbuf(rlog, nullptr, _IOFBF, 1 << 20);
	recompute();
}
void tracer_replay_close() { if (rlog) { pr_flush(); if (rlogPipe) pclose(rlog); else fclose(rlog); rlog = nullptr; } recompute(); }
uint64_t tracer_prog_count() { return progCount; }
void tracer_replay_frame(int frame) {
	if (!rlog) return;
	pr_flush();
	fprintf(rlog, "F %d %llu %d %04X:%04X cx=%04X\n", frame, (unsigned long long)progCount, inProg ? 1 : 0, SegValue(cs), (unsigned)(reg_eip & 0xFFFF), (unsigned)(reg_ecx & 0xFFFF));
}
// the program's side of the line at cs:ip (before the instruction there runs): crossing it is an excursion's start
// (X) or end (R), or a handler's entry or return when an interrupt was taken outside
static bool replay_sync(uint16_t curCs, uint16_t curIp) {
	const bool p = curCs >= progLo && curCs < progHi;
	if (irqOutside && !p) irqOutside = false;   // (that interrupt's handler is not the program's)
	if (p && !inProg) {   // into the program: a handler of an interrupt taken outside, or the end of an excursion
		if (irqOutside) { levels.push_back({'H', irqCs, irqIp}); irqOutside = false; }
		else if (!levels.empty() && levels.back().kind == 'E') { pr_flush(); log_regs('R'); levels.pop_back(); }
	} else if (!p && inProg) {   // out of the program: a handler returning into its excursion, or a new excursion
		if (!levels.empty() && levels.back().kind == 'H' && levels.back().cs == curCs && levels.back().ip == curIp) levels.pop_back();
		else if (progCount) { pr_flush(); log_regs('X'); levels.push_back({'E', 0, 0}); }
	}
	inProg = p;
	return p;
}
void tracer_irq(unsigned vector) {
	if (!rlog) return;
	// the side of the line where the interrupt finds the program (the last instruction may have crossed it)
	replay_sync(SegValue(cs), (uint16_t)reg_eip);
	if (inProg) ram_at_check();
	pr_flush();
	TracerRegs r; tracer_read_regs(&r);
	// (then the other registers: the replay checks them where the program was, and runs a handler of an interrupt
	// taken outside on the stack where it ran)
	fprintf(rlog, "I %llu %02X %04X:%04X cx=%04X fl=%04X %d ax=%04X bx=%04X dx=%04X si=%04X di=%04X bp=%04X sp=%04X ds=%04X es=%04X ss=%04X\n",
		(unsigned long long)progCount, vector & 0xFF, r.cs, r.eip & 0xFFFF, r.ecx & 0xFFFF, r.eflags & 0xFFFF, inProg ? 1 : 0,
		r.eax & 0xFFFF, r.ebx & 0xFFFF, r.edx & 0xFFFF, r.esi & 0xFFFF, r.edi & 0xFFFF, r.ebp & 0xFFFF, r.esp & 0xFFFF, r.ds, r.es, r.ss);
	if (!inProg && progCount) { irqOutside = true; irqCs = r.cs; irqIp = (uint16_t)r.eip; }
}
void tracer_port_read(unsigned port, unsigned width, uint32_t value) {
	if (!rlog || !inProg) return;
	const uint16_t c = SegValue(cs), i = (uint16_t)reg_eip;
	if (prPending && pr.port == port && pr.width == width && pr.value == value && pr.cs == c && pr.ip == i) { pr.n++; pr.last = progCount; return; }
	pr_flush();
	pr.port = port; pr.width = width; pr.value = value; pr.cs = c; pr.ip = i; pr.pos = pr.last = progCount; pr.n = 1; prPending = true;
}
void tracer_rep_restart() { repRestart = true; repCs = SegValue(cs); repIp = (uint16_t)reg_eip; }

static TracerEvent &push() {
	if (ring.size() != cap) { ring.assign(cap, TracerEvent{}); head = count = 0; }
	uint32_t idx = (head + count) % cap;
	if (count == cap) { head = (head + 1) % cap; dropped++; } else count++;
	TracerEvent &e = ring[idx]; memset(&e, 0, sizeof e); return e;
}

void tracer_read_regs(TracerRegs *r) {
	FillFlags();
	r->eax = reg_eax; r->ebx = reg_ebx; r->ecx = reg_ecx; r->edx = reg_edx;
	r->esi = reg_esi; r->edi = reg_edi; r->ebp = reg_ebp; r->esp = reg_esp;
	r->eip = reg_eip; r->eflags = reg_flags;
	r->cs = SegValue(cs); r->ds = SegValue(ds); r->es = SegValue(es);
	r->ss = SegValue(ss); r->fs = SegValue(fs); r->gs = SegValue(gs);
}

static uint32_t readN(uint32_t phys, uint32_t len) {
	uint32_t v = 0;
	for (uint32_t i = 0; i < len; i++) v |= (uint32_t)phys_readb(phys + i) << (8 * i);
	return v;
}

static void apply_regs(const TracerRegs &r) {
	reg_eax = r.eax; reg_ebx = r.ebx; reg_ecx = r.ecx; reg_edx = r.edx;
	reg_esi = r.esi; reg_edi = r.edi; reg_ebp = r.ebp; reg_esp = r.esp; reg_eip = r.eip;
	SegSet16(cs, r.cs); SegSet16(ds, r.ds); SegSet16(es, r.es); SegSet16(ss, r.ss); SegSet16(fs, r.fs); SegSet16(gs, r.gs);
	reg_flags = r.eflags; lflags.type = t_UNKNOWN;
}
void tracer_inject(uint16_t whenCs, uint16_t whenIp, const TracerRegs &regs) { injWhenCs = whenCs; injWhenIp = whenIp; injRegs = regs; injState = 1; recompute(); }
int tracer_inject_state() { return injState; }
void tracer_poke(uint32_t phys, const uint8_t *data, uint32_t len) { for (uint32_t i = 0; i < len; i++) phys_writeb(phys + i, data[i]); }

// the machine as the program starts (its first instruction): the 640 KB in 64-byte records of the chunks not all zero
// ("M phys hex") and the registers ("S"), for the replay to check its own start against
static void replay_start_state() {
	static const char hexd[] = "0123456789ABCDEF";
	char line[160];
	for (uint32_t a = 0; a < 0xA0000; a += 64) {
		uint8_t b[64]; bool any = false;
		for (int k = 0; k < 64; k++) { b[k] = phys_readb(a + k); any = any || b[k]; }
		if (!any) continue;
		int n = snprintf(line, sizeof line, "M %05X ", a);
		for (int k = 0; k < 64; k++) { line[n++] = hexd[b[k] >> 4]; line[n++] = hexd[b[k] & 15]; }
		line[n++] = '\n'; line[n] = 0;
		fputs(line, rlog);
	}
	log_regs('S');
}

void tracer_hook() {
	const uint16_t curCs = SegValue(cs), curIp = (uint16_t)reg_eip;
	if (rlog) {
		const bool p = replay_sync(curCs, curIp);
		if (p && progCount == 0) replay_start_state();
		if (p) {
			if (repRestart && curCs == repCs && curIp == repIp) repRestart = false;   // (a string op resuming: counted when it began)
			else { ram_at_check(); progCount++; }   // (a dump not at a resumption: where the core broke the string op is its own)
		}
	}
	if (injState == 1 && curCs == injWhenCs && curIp == injWhenIp) {
		tracer_read_regs(&injSaved);
		TracerRegs r = injRegs;
		if (r.ss == 0xFFFF) { r.ss = injSaved.ss; r.esp = (injSaved.esp - 0x400) & 0xFFFF; }
		r.eflags = injSaved.eflags; r.fs = injSaved.fs; r.gs = injSaved.gs; r.ebp = r.esp;
		r.esp = (r.esp - 4) & 0xFFFF;
		phys_writew(PhysMake(r.ss, r.esp), 0); phys_writew(PhysMake(r.ss, r.esp + 2), 0); // far return to 0000:0000
		apply_regs(r); injState = 2;
		TracerEvent &e = push(); e.kind = TRACER_INJECT_START; e.cs = curCs; e.ip = curIp; e.instrCount = instrCount; e.regs = r;
		return;
	}
	if (injState == 2 && curCs == 0 && curIp == 0) {
		TracerEvent &e = push(); e.kind = TRACER_INJECT_DONE; e.instrCount = instrCount; tracer_read_regs(&e.regs);
		apply_regs(injSaved); injState = 3; recompute();
		return;
	}
	// watchpoints: compare against the value seen after the previous instruction
	for (uint32_t w = 0; w < watches.size(); w++) {
		uint32_t v = readN(watches[w].phys, watches[w].len);
		if (v != watches[w].last) {
			TracerEvent &e = push(); e.kind = TRACER_WATCH; e.id = (uint8_t)w; e.cs = curCs; e.ip = curIp;
			e.addr = watches[w].phys; e.oldVal = watches[w].last; e.newVal = v; e.instrCount = instrCount;
			watches[w].last = v;
		}
	}
	for (uint32_t p = 0; p < probes.size(); p++) {
		if (probes[p].eip32 ? probes[p].eip == reg_eip : (probes[p].cs == curCs && probes[p].ip == curIp)) {
			TracerEvent &e = push(); e.kind = TRACER_PROBE; e.id = (uint8_t)p; e.cs = curCs; e.ip = curIp; e.instrCount = instrCount;
			tracer_read_regs(&e.regs);
			PhysPt sp = SegPhys(ss) + (probes[p].eip32 ? reg_esp : reg_sp);   // flat protected mode: 32-bit stack
			for (int i = 0; i < 32; i++) e.stack[i] = phys_readb(sp + i);
			e.memOff = (uint32_t)arena.size(); e.memLen = probes[p].len;
			// a sample address 0xFFFFxxxx is relative to ss:sp, 0xFFFExxxx to ds:0 (the low 16 bits the offset)
			PhysPt mb = (probes[p].phys >> 16) == 0xFFFF ? sp + (probes[p].phys & 0xFFFF) : (probes[p].phys >> 16) == 0xFFFE ? SegPhys(ds) + (probes[p].phys & 0xFFFF) : probes[p].phys;
			for (uint32_t i = 0; i < probes[p].len; i++) { uint8_t b = phys_readb(mb + i); arena.push_back(b); if (i < 64) e.mem[i] = b; }
			PhysPt dx = SegPhys(ds) + (reg_edx & 0xFFFF);
			for (int i = 0; i < 32; i++) e.dsdx[i] = phys_readb(dx + i);
			if (probeHits.size() < probes.size()) probeHits.resize(probes.size(), 0);
			uint32_t hit = ++probeHits[p];
			for (const ProbePoke &pp : probePokes) if (pp.probe == (int)p && pp.hit == hit) for (uint32_t i = 0; i < pp.bytes.size(); i++) phys_writeb(pp.phys + i, pp.bytes[i]);
		}
	}
	if (logInstr) {
		TracerEvent &e = push(); e.kind = TRACER_INSTR; e.cs = curCs; e.ip = curIp; e.instrCount = instrCount;
		PhysPt a = SegPhys(cs) + reg_eip;
		for (int i = 0; i < 8; i++) e.opcode[i] = phys_readb(a + i);
		e.regs.eax = reg_eax; e.regs.esp = reg_esp; e.regs.ds = SegValue(ds);
	}
	instrCount++;
}

int tracer_add_probe(uint16_t cs, uint16_t ip) {
	if (probes.size() >= 64) return -1;
	probes.push_back({cs, ip, 0, 0}); recompute(); return (int)probes.size() - 1;
}
int tracer_add_probe_mem(uint16_t cs, uint16_t ip, uint32_t phys, uint32_t len) {
	if (probes.size() >= 64 || len > 65536) return -1;
	probes.push_back({cs, ip, phys, len}); recompute(); return (int)probes.size() - 1;
}
int tracer_add_probe32(uint32_t eip, uint32_t phys, uint32_t len) {
	if (probes.size() >= 64 || len > 65536) return -1;
	Probe pr{0, (uint16_t)eip, phys, len}; pr.eip32 = true; pr.eip = eip;
	probes.push_back(pr); recompute(); return (int)probes.size() - 1;
}
int tracer_add_watch(uint32_t phys, uint32_t len) {
	if (watches.size() >= 64 || len < 1 || len > 4) return -1;
	watches.push_back({phys, len, readN(phys, len)}); recompute(); return (int)watches.size() - 1;
}
void tracer_clear() { probes.clear(); probeHits.clear(); probePokes.clear(); watches.clear(); logInstr = false; head = count = dropped = 0; recompute(); }
void tracer_log_instructions(bool on) { logInstr = on; recompute(); }
void tracer_set_capacity(uint32_t n) { cap = n ? n : 1; ring.clear(); head = count = 0; }
uint32_t tracer_event_count() { return count; }
uint32_t tracer_events_dropped() { return dropped; }
const TracerEvent *tracer_event(uint32_t i) { return i < count ? &ring[(head + i) % cap] : nullptr; }
void tracer_events_clear() { head = count = 0; dropped = 0; arena.clear(); }
const uint8_t *tracer_event_mem(const TracerEvent *e, uint32_t *len) { if (len) *len = e->memLen; return e->memLen && e->memOff + e->memLen <= arena.size() ? arena.data() + e->memOff : e->mem; }
uint64_t tracer_instr_count() { return instrCount; }
void tracer_probe_poke(int probe, uint32_t hit, uint32_t phys, const uint8_t *data, uint32_t len) { probePokes.push_back({probe, hit, phys, std::vector<uint8_t>(data, data + len)}); }
