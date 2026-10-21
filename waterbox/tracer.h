// Execution tracer for the DOSBox-X core: per-instruction probes, memory watchpoints and an
// instruction log, recorded into a ring buffer without pausing the machine. Hooked from the
// normal CPU core (patches/src/cpu/core_normal.cpp) through tracer_hook(); costs one branch per
// instruction when idle. Pure memory, no I/O, so it is the same in the native and guest builds.
#pragma once
#include <cstdint>

struct TracerRegs {
	uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags;
	uint16_t cs, ds, es, ss, fs, gs;
};

enum TracerEventKind : uint8_t { TRACER_PROBE = 1, TRACER_WATCH = 2, TRACER_INSTR = 3, TRACER_INJECT_START = 4, TRACER_INJECT_DONE = 5 };

struct TracerEvent {
	uint8_t kind;
	uint8_t id;            // probe / watch index
	uint16_t cs, ip;       // where the CPU was
	uint32_t addr;         // watch: physical address written
	uint32_t oldVal, newVal; // watch: bytes before/after (little-endian, up to 4)
	uint64_t instrCount;   // global instruction counter at the event
	TracerRegs regs;       // probe: the register file at entry
	uint8_t stack[32];     // probe: bytes at ss:sp
	uint8_t dsdx[32];      // probe: bytes at ds:dx (DOS call buffers / filenames)
	uint8_t opcode[8];     // instr: opcode bytes at cs:ip
	uint8_t mem[64];       // probe: bytes at the probe's sample address (tracer_add_probe_mem), first 64
	uint32_t memOff, memLen; // probe: the whole sample (up to 64 KiB) in the side arena, see tracer_event_mem()
};

extern bool tracer_active;            // any probe/watch/log armed
void tracer_hook();                   // called before every instruction by the CPU core

int  tracer_add_probe(uint16_t cs, uint16_t ip);       // returns probe id (< 64) or -1
int  tracer_add_probe_mem(uint16_t cs, uint16_t ip, uint32_t phys, uint32_t len);
int  tracer_add_probe32(uint32_t eip, uint32_t phys, uint32_t len); // flat protected mode: full EIP, any CS; len 0 = no sample // same, plus a memory sample (len <= 65536)
int  tracer_add_watch(uint32_t phys, uint32_t len);    // len 1..4; returns watch id or -1
void tracer_clear();                                   // drop probes, watches, log, events
void tracer_log_instructions(bool on);                 // record every instruction as TRACER_INSTR
void tracer_set_capacity(uint32_t events);             // ring size (default 1<<20)

uint32_t tracer_event_count();            // events currently held (oldest dropped when full)
uint32_t tracer_events_dropped();
const TracerEvent *tracer_event(uint32_t i);
const uint8_t *tracer_event_mem(const TracerEvent *e, uint32_t *len); // a probe's whole sample (valid until tracer_events_clear)
void tracer_events_clear();
uint64_t tracer_instr_count();
void tracer_read_regs(TracerRegs *out);   // current register file
// Call injection: the next time execution reaches whenCs:whenIp, save the register file, load
// `regs` (cs:ip = target; ss==0xFFFF means "current stack, sp - 0x400"), push a far return to
// 0000:0000 and run. When execution reaches 0000:0000 the saved registers are restored (the
// return value registers are recorded in a TRACER_INJECT_DONE event). One injection at a time.
void tracer_inject(uint16_t whenCs, uint16_t whenIp, const TracerRegs &regs);
int  tracer_inject_state();               // 0 idle, 1 waiting for the safe point, 2 running, 3 done
void tracer_poke(uint32_t phys, const uint8_t *data, uint32_t len); // write guest memory
// Write guest memory when probe `probe` fires for the `hit`-th time (1-based), right after its sample is taken:
// input synchronized with the program (e.g. a game's per-tick key table) instead of with video frames.
void tracer_probe_poke(int probe, uint32_t hit, uint32_t phys, const uint8_t *data, uint32_t len);

// Replay log (oracle-run --replay PATH --prog LO HI): the program's own instructions are counted (CS in [LO, HI),
// a REP string instruction once however often it restarts), and each thing the program gets from outside is
// written with that count, in order: the hardware interrupts it takes, the ports it reads, its excursions out of its
// own code (BIOS / DOS services: the registers on the way out and back), the frame ends. A recompilation run with
// the same count replays them and must arrive at the same states. Records (one a line):
//   M phys hex           64 bytes of the memory as the program starts (the chunks not all zero), then
//   S 0 cs:ip ax=..      the registers there
//   I pos vec cs:ip cx= fl= inProg ax= bx= dx= si= di= bp= sp= ds= es= ss=   a hardware interrupt taken
//   P pos port width value cs:ip n last   n reads of a port by one instruction, one after another (the first at
//                        pos, the last at last)
//   X pos cs:ip ax= .. fl=, R ..   out of the program (a service's entry) and back
//   F frame pos inProg cs:ip cx=   a frame's end
void tracer_replay_open(const char *path, uint16_t lo, uint16_t hi);
void tracer_replay_close();
void tracer_replay_frame(int frame);
void tracer_replay_ram_at(uint64_t pos, const char *path);           // the 640 KB after the program's instruction POS (its next instruction's or an interrupt's count)
uint64_t tracer_prog_count();
void tracer_irq(unsigned vector);                                      // pic.cpp: a hardware interrupt is about to be taken
void tracer_port_read(unsigned port, unsigned width, uint32_t value);  // iohandler.cpp: a port read
void tracer_rep_restart();                                             // string.h: a REP string op stops early, will run again
