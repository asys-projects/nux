/*
  NUX: A kernel Library.
  Copyright (C) 2019 Gianluca Guida, glguida@tlbflush.org

  SPDX-License-Identifier:	BSD-2-Clause
*/

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <nux/hal.h>
#include <nux/plt.h>

#include "i386.h"
#include "../internal.h"

#if 0
static char *exceptions[] = {
  "Divide by zero exception",
  "Debug exception",
  "NMI",
  "Overflow exception",
  "Breakpoint exception",
  "Bound range exceeded",
  "Invalid opcode",
  "No math coprocessor",
  "Double fault",
  "Coprocessor segment overrun",
  "Invalid TSS",
  "Segment not present",
  "Stack segment fault",
  "General protection fault",
  "Page fault",
  "Reserved exception",
  "Floating point error",
  "Alignment check fault",
  "Machine check fault",
  "SIMD Floating-Point Exception",
};
#endif


struct hal_frame *
do_nmi (uint32_t vect, struct hal_frame *f)
{

  hal_entry_nmi (f);
  return f;
}

struct hal_frame *
do_xcpt (uint32_t vect, struct hal_frame *f)
{
  struct hal_frame *rf;

  if (vect == 8)
    {
      nux_panic ("DOUBLE FAULT EXCEPTION:\n", f);
    }
  else if (vect == 18)
    {
      nux_panic ("MACHINE CHECK EXCEPTION:\n", f);
    }
  else if (vect == 14)
    {
      unsigned xcpterr;

      /* Page Fault. */

      if (f->err & 1)
	{
	  xcpterr = HAL_PF_REASON_PROT;
	}
      else
	{
	  xcpterr = HAL_PF_REASON_NOTP;
	}

      if (f->err & 2)
	xcpterr |= HAL_PF_INFO_WRITE;

      if (f->err & 4)
	xcpterr |= HAL_PF_INFO_USER;

      if (f->err & 16)
	xcpterr |= HAL_PF_INFO_EXE;

      rf = hal_entry_pf (f, f->cr2, xcpterr);
    }
  else
    {
      rf = hal_entry_xcpt (f, vect);
    }
  return rf;
}

struct hal_frame *
do_syscall (struct hal_frame *f)
{
  struct hal_frame *rf;

  assert (f->cs == UCS);

  rf =
    hal_entry_syscall (f, f->eax, f->edi, f->esi, f->ecx, f->edx, f->ebx,
		       f->ebp);
  return rf;
}

/*
 * Return the location from which to resume frame RF.  User frames not
 * already at the top of this CPU's kernel stack are copied there (see
 * entry.S).  Kernel frames are always the entry frame, on the stack.
 */
struct hal_frame *
i386_frame_resume (struct hal_frame *rf)
{
  extern vaddr_t pcpu_haldata[];
  struct hal_frame *top;
  struct hal_cpu *hc;
  uint16_t tr;

  if (rf->cs != UCS)
    return rf;
  /* TSS selector of CPU n is (5 + 4 * n) << 3 (see hal_pcpu_enter). */
  asm volatile ("str %0":"=r" (tr));
  hc = (struct hal_cpu *) (uintptr_t) pcpu_haldata[((tr >> 3) - 5) / 4];
  top = (struct hal_frame *) (uintptr_t) (hc->tss.esp0 - sizeof (*top));
  if (top != rf)
    memcpy (top, rf, sizeof (*top));
  return top;
}

struct hal_frame *
do_vect (uint32_t vect, struct hal_frame *f)
{
  return plt_interrupt (vect, f);
}

void
hal_frame_init (struct hal_frame *f)
{
  memset (f, 0, sizeof (*f));
  f->eip = 0;
  f->esp = 0;

  f->cs = UCS;
  f->ds = UDS;
  f->es = UDS;
  f->fs = UDS;
  f->gs = UDS;
  f->ss = UDS;

  f->eflags = 0x202;
}

bool
hal_frame_isuser (struct hal_frame *f)
{
  return f->cs == UCS;
}

vaddr_t
hal_frame_getip (struct hal_frame *f)
{
  return f->eip;
}

void
hal_frame_setip (struct hal_frame *f, vaddr_t ip)
{
  f->eip = ip;
}

vaddr_t
hal_frame_getsp (struct hal_frame *f)
{
  return f->esp;
}

void
hal_frame_setsp (struct hal_frame *f, vaddr_t sp)
{
  f->esp = sp;
}

vaddr_t
hal_frame_getgp (struct hal_frame *f)
{
  return 0;
}

void
hal_frame_setgp (struct hal_frame *f, unsigned long gp)
{
  /* Do nothing. */
}

void
hal_frame_settls (struct hal_frame *f, unsigned long tls)
{
  /*
    i386 TLS requires setting a LDT for each process and it is in
    general very different to what modern architectures do.

    It is possible to add TLS support to the i386 HAL. For now, we
    just ignore the call.
  */
}

void
hal_frame_seta0 (struct hal_frame *f, unsigned long a0)
{
  f->eax = a0;
}

void
hal_frame_seta1 (struct hal_frame *f, unsigned long a1)
{
  f->edx = a1;
}

void
hal_frame_seta2 (struct hal_frame *f, unsigned long a2)
{
  f->ecx = a2;
}

void
hal_frame_setret (struct hal_frame *f, unsigned long r)
{
  f->eax = r;
}

void
hal_frame_print (struct hal_frame *f)
{

  hallog ("EAX: %08x EBX: %08x ECX: %08x EDX:%08x",
	  f->eax, f->ebx, f->ecx, f->edx);
  hallog ("EDI: %08x ESI: %08x EBP: %08x ESP:%08x",
	  f->edi, f->esi, f->ebp, f->esp);
  hallog (" CS: %04x     EIP: %08x EFL: %08x",
	  (int) f->cs, f->eip, f->eflags);
  hallog (" DS: %04x      ES: %04x     FS: %04x      GS: %04x",
	  f->ds, f->es, f->fs, f->gs);
  hallog ("CR3: %08x CR2: %08x err: %08x", f->cr3, f->cr2, f->err);
}

unsigned long
frame_bp (struct hal_frame *f)
{
  return f->ebp;
}

unsigned long
frame_cr2 (struct hal_frame *f)
{
  return f->cr2;
}
