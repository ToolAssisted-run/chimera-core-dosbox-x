/*
  libco for aarch64, in the shape of amd64.c beside it: a cothread's stack
  is mmap'd with MAP_STACK and a guard below it, and co_derive and
  serialisation are not offered. The switch keeps what AAPCS64 says a callee
  must keep (sp, x19-x30, d8-d15) and never touches x18, the platform's
  register. It is the switch of ares's libco/aarch64.c (ISC licence,
  Copyright (c) 2004-2025 ares team, Near et al), written as assembly.
*/

#include "libco.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// allocations are 16k larger than asked for, which is all used as guard space
#define GUARD_SIZE 0x4000

typedef struct {
	// used by libco_aarch64_swap, has to be at the beginning of the struct:
	// sp, x30, x19-x28, x29, d8-d15 (offsets 0, 8, 16-88, 96, 112-168)
	uint64_t regs[22];
	// points to the lowest address in the stack
	// NB: because of guard space, this is not valid stack
	void* stack;
	// length of the stack that we allocated in bytes
	uint64_t stack_size;
} cothread_impl;

// void libco_aarch64_swap(cothread_impl* to, cothread_impl* from)
__asm__(
	".text\n"
	".globl libco_aarch64_swap\n"
	".type libco_aarch64_swap,%function\n"
	"libco_aarch64_swap:\n"
	"	mov x16, sp\n"
	"	stp x16, x30, [x1]\n"
	"	ldp x16, x30, [x0]\n"
	"	mov sp, x16\n"
	"	stp x19, x20, [x1, 16]\n"
	"	ldp x19, x20, [x0, 16]\n"
	"	stp x21, x22, [x1, 32]\n"
	"	ldp x21, x22, [x0, 32]\n"
	"	stp x23, x24, [x1, 48]\n"
	"	ldp x23, x24, [x0, 48]\n"
	"	stp x25, x26, [x1, 64]\n"
	"	ldp x25, x26, [x0, 64]\n"
	"	stp x27, x28, [x1, 80]\n"
	"	ldp x27, x28, [x0, 80]\n"
	"	str x29, [x1, 96]\n"
	"	ldr x29, [x0, 96]\n"
	"	stp d8, d9, [x1, 112]\n"
	"	ldp d8, d9, [x0, 112]\n"
	"	stp d10, d11, [x1, 128]\n"
	"	ldp d10, d11, [x0, 128]\n"
	"	stp d12, d13, [x1, 144]\n"
	"	ldp d12, d13, [x0, 144]\n"
	"	stp d14, d15, [x1, 160]\n"
	"	ldp d14, d15, [x0, 160]\n"
	"	br x30\n"
	".size libco_aarch64_swap, .-libco_aarch64_swap\n");

void libco_aarch64_swap(cothread_impl* to, cothread_impl* from);

// the cothread that represents the real host thread we started from
static cothread_impl co_host_buffer;
// what cothread are we in right now
static cothread_impl* co_active_handle;

static void free_thread(cothread_impl* co)
{
	if (munmap(co->stack, co->stack_size) != 0)
		abort();
	free(co);
}

static cothread_impl* alloc_thread(uint64_t size)
{
	cothread_impl* co = calloc(1, sizeof(*co));
	if (!co)
		return NULL;

	// align up to 4k
	size = (size + 4095) & ~4095ul;
	size += GUARD_SIZE;

	co->stack = mmap(NULL, size,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);

	if (co->stack == (void*)(-1))
	{
		free(co);
		return NULL;
	}

	if (mprotect(co->stack, GUARD_SIZE, PROT_NONE) != 0)
	{
		free_thread(co);
		return NULL;
	}

	co->stack_size = size;
	return co;
}

// the first switch into a cothread lands here, with x0 the cothread (the
// switch's first argument) and its entry point in the x19 slot
static void co_entrypoint(cothread_impl* co)
{
	void (*entrypoint)(void) = (void (*)(void))co->regs[2];
	entrypoint();
	abort(); // called only if cothread_t entrypoint returns
}

void co_clean(void)
{
	memset(&co_host_buffer, 0, sizeof(co_host_buffer));
}

cothread_t co_active(void)
{
	if (!co_active_handle)
		co_active_handle = &co_host_buffer;
	return co_active_handle;
}

cothread_t co_create(unsigned int sz, void (*entrypoint)(void))
{
	cothread_impl* co;
	if (!co_active_handle)
		co_active_handle = &co_host_buffer;

	if ((co = alloc_thread(sz)))
	{
		uint64_t top = ((uint64_t)co->stack + co->stack_size) & ~(uint64_t)15; // top of stack
		co->regs[0] = top;                            // sp
		co->regs[1] = (uint64_t)(uintptr_t)co_entrypoint; // x30
		co->regs[2] = (uint64_t)(uintptr_t)entrypoint;    // x19
		co->regs[12] = top;                           // x29, the frame pointer
	}

	return co;
}

void co_delete(cothread_t handle)
{
	free_thread(handle);
}

void co_switch(cothread_t handle)
{
	cothread_impl* co = handle;
	cothread_impl* co_previous_handle = co_active_handle;
	co_active_handle = co;
	libco_aarch64_swap(co, co_previous_handle);
}

cothread_t co_derive(void* memory, unsigned sz, void (*entrypoint)(void))
{
	return NULL;
}

int co_serializable(void)
{
	return 0;
}
