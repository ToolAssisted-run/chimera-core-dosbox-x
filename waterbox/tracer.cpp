#include "tracer.h"
#include <vector>
#include <cstring>
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

struct Probe { uint16_t cs, ip; uint32_t phys, len; };
struct Watch { uint32_t phys, len, last; };
static std::vector<Probe> probes;
static std::vector<uint32_t> probeHits;
struct ProbePoke { int probe; uint32_t hit, phys; std::vector<uint8_t> bytes; };
static std::vector<ProbePoke> probePokes;   // sorted by (probe, hit) as added; applied when the hit comes
static std::vector<Watch> watches;

static void recompute() { tracer_active = logInstr || !probes.empty() || !watches.empty() || injState == 1 || injState == 2; }

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

void tracer_hook() {
	const uint16_t curCs = SegValue(cs), curIp = (uint16_t)reg_eip;
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
		if (probes[p].cs == curCs && probes[p].ip == curIp) {
			TracerEvent &e = push(); e.kind = TRACER_PROBE; e.id = (uint8_t)p; e.cs = curCs; e.ip = curIp; e.instrCount = instrCount;
			tracer_read_regs(&e.regs);
			PhysPt sp = SegPhys(ss) + reg_sp;
			for (int i = 0; i < 32; i++) e.stack[i] = phys_readb(sp + i);
			e.memOff = (uint32_t)arena.size(); e.memLen = probes[p].len;
			for (uint32_t i = 0; i < probes[p].len; i++) { uint8_t b = phys_readb(probes[p].phys + i); arena.push_back(b); if (i < 64) e.mem[i] = b; }
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
