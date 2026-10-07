/*
  NUX: A kernel Library.
  Copyright (C) 2019 Gianluca Guida <glguida@tlbflush.org>

  SPDX-License-Identifier:	BSD-2-Clause
*/

#ifndef _HAL_X86_CONFIG
#define _HAL_X86_CONFIG

#ifndef _ASSEMBLER

#include <stdint.h>

/* An L1 pointer is either a kernel virtual address (page tables of the
   current address space) or, tagged by bit 0, the physical address of
   an entry in a foreign table.  With PAE on i386 page tables can live
   above 4GB, so the type must hold 64-bit physical addresses. */
#ifdef __i386__
typedef uint64_t hal_l1p_t;
#else
typedef uintptr_t hal_l1p_t;
#endif
#define L1P_INVALID ((hal_l1p_t)0)
typedef uint64_t hal_l1e_t;

/*
  FPU registers: the FXSAVE area (x87, MMX and SSE state), or the
  FNSAVE area on CPUs without FXSR.  #NM (device not available) is
  raised by the first FPU instruction when the FPU is disabled
  (CR0.TS).
*/
struct hal_fpu
{
  uint8_t area[512];
} __attribute__ ((aligned (16)));

#define HAL_XCPT_FPU 7

/*
  x86 specific: a 32-bit read of I/O port REGS->edx with the general
  registers as inputs and outputs of the instruction, as hypervisor
  backdoors define it (VMware's: EAX magic, EBX argument, ECX command,
  all four registers returned).
*/
struct hal_x86_ioregs
{
  uint32_t eax, ebx, ecx, edx;
};

void hal_x86_inl_regs (struct hal_x86_ioregs *regs);

static inline void
hal_debug (void)
{
  asm volatile ("int3\n\t");
}

#endif

#ifdef __i386__
#include "hal_config_i386.h"
#endif

#ifdef __amd64__
#include "hal_config_amd64.h"
#endif

#endif
