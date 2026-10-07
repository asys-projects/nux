/*
  NUX: A kernel Library.
  Copyright (C) 2019 Gianluca Guida, glguida@tlbflush.org

  SPDX-License-Identifier:	BSD-2-Clause
*/

#include <machine/asm.h>
#include "../internal.h"
#include "i386.h"

	.text
	.globl _start
_start:
	mov %esi, %cr3
	movl $_bsp_stacktop, %esp

	/* Clear flags (cld and cli) */
	pushl $0
	popf

	/* Set segments in a known state */
	call _setup_gdt
	lgdt _gdtr

	ljmp $KCS, $1f
1:	mov $KDS, %eax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %ss
	mov $0, %eax
	mov %ax, %fs
	mov %ax, %gs

	mov $(5 << 3), %ax
	ltr %ax

	/* Setup IDT */
	call _setup_idt
	lidt _idtr

	call _clear_bss

	// Save CR3 for future AP boots.
	mov %cr3, %eax
	mov %eax, _bsp_cr3

	/* init hal */
	call _C_LABEL(x86_init)

	push $_init_string
	call ___start
loop:
	cli
	hlt
	jmp loop

_clear_bss:
	push %edi
	mov $0x0, %eax
	mov $_ebss, %ecx
	sub $_sbss, %ecx
	mov $_sbss, %edi
	rep stosb
	pop %edi
	ret

_setup_gdt:
	movl $_gdt + 8, %eax
	/* 32-bit code */
	movl $0x0000ffff, (%eax)
	movl $0x00cf9a00, 4(%eax)
	add $8, %eax
	/* 32-bit data */
	movl $0x0000ffff, (%eax)
	movl $0x00cf9200, 4(%eax)
	add $8, %eax
	/* User 32-bit code */
	movl $0x0000ffff, (%eax)
	movl $0x00cffa00, 4(%eax)
	add $8, %eax
	/* User 32-bit data */
	movl $0x0000ffff, (%eax)
	movl $0x00cff200, 4(%eax)
	add $8, %eax
	/* TSS */
	movl $0, (%eax)
	movl $0, 4(%eax)
	movl _tss, %ecx
	movw %cx, 2(%eax)
	movw $104, (%eax)
	shr  $16, %ecx
	movb %cl, 4(%eax)
	movb %ch, 7(%eax)
	orl  $0x00408900, 4(%eax)
	ret

	.altmacro
.macro repeat macro from to
	\macro \from
	.if \to-\from
	repeat \macro,%(\from+1),\to
	.endif
.endm

.macro _setidt vct usr
	mov $hdlr_&\vct, %ecx
	mov $KCS, %ax
	shl $16, %eax
	mov %cx, %ax
	mov %eax, _idt + (\vct * 8)
	mov %ecx, %eax
	.if \usr > 0
	mov $0xee00, %ax
	.else
	mov $0x8e00, %ax
	.endif
	mov %eax, _idt + (\vct * 8) + 4
.endm

.macro setidt vct
	_setidt \vct 0
.endm

_setup_idt:
	repeat setidt 0, 31
	repeat setidt 32, 63
	repeat setidt 64, 95
	repeat setidt 96, 127
	repeat setidt 128, 191
	repeat setidt 192, 255
	_setidt 33 1 /* syscall */
	ret
	

	.globl _C_LABEL(_set_tss)
_C_LABEL(_set_tss):
	push  %ebp
	mov   %esp, %ebp
	/* TSS (5 + 4*n) */
	movl  8(%ebp), %eax
	shl   $5, %eax
	add $_gdt + (5 * 8), %eax
	movl $0, (%eax)
	movl $0, 4(%eax)
	movl 0xc(%ebp), %ecx
	movw $104, (%eax)
	movw %cx, 2(%eax)
	shr  $16, %ecx
	movb %cl, 4(%eax)
	movb %ch, 7(%eax)
	orl  $0x00408900, 4(%eax)
	pop  %ebp
	ret

	.globl _C_LABEL(_set_fs)
_C_LABEL(_set_fs):
	push  %ebp
	mov   %esp, %ebp
	/* FS (5 + 4*n + 1)*/
	mov  8(%ebp), %ecx
	mov  %ecx, %eax
	shl $5, %eax
	add $_gdt + ((5 + 1) * 8), %eax
	movl $0, (%eax)
	movl $0, 4(%eax)
	movw $4, (%eax)
	movl 0xc(%ebp), %ecx
	movw %cx, 2(%eax)
	shr  $16, %ecx
	movb %cl, 4(%eax)
	movb %ch, 7(%eax)
	orl  $0x00409200, 4(%eax)
	pop  %ebp
	ret

	.globl _C_LABEL(_set_gs)
_C_LABEL(_set_gs):
	push  %ebp
	mov   %esp, %ebp
	/* GS (5 + 4*n + 2)*/
	mov  8(%ebp), %eax
	shl $5, %eax
	add $_gdt + ((5 + 2) * 8), %eax
	movl $0, (%eax)
	movl $0, 4(%eax)
	movw $0xff, (%eax)
	movl 0xc(%ebp), %ecx
	movw %cx, 2(%eax)
	shr  $16, %ecx
	movb %cl, 4(%eax)
	movb %ch, 7(%eax)
	orl  $0x0040f200, 4(%eax)
	pop  %ebp
	ret

	
/*
 * AP BOOTSTRAP.
 *
 * This code is copied into per-cpu pages.
 * These same page will become the AP kernel
 * stack.
 */
	.code16
	.globl _C_LABEL(_ap_start)
ENTRY(_ap_start)
	cli
	movw %cs, %ax
	movw %ax, %ds

	lgdtl _ap_gdtreg - _C_LABEL(_ap_start)
	/* No IDT. If anything goes wrong here, machine triple-faults. */

	movw $0xb800, %ax
	movw %ax, %es
	movw $('A'<<8 + 'A'), %es:0

	/* Save AP Boostrap Page Segment. */
	mov %cs, %bx

	/* Enter protected mode. */
	mov $1, %ax
	lmsw %ax
	ljmpl _ap_ljmp - _ap_start

	.align 16
.Lap_gdttmp:
	.long 0
	.long 0
	.long 0x0000ffff
	.long 0x00cf9a00
	.long 0x0000ffff
	.long 0x00cf9200

	/*
	  AP Bootstrap configuration.

	  The following fields are set up by prepare.
	*/
	.globl _ap_gdtreg, _ap_ljmp, _ap_stackpage, _ap_cr3
_ap_gdtreg:
	.hword 127
	.long .Lap_gdttmp - _C_LABEL(_ap_start)
_ap_ljmp:
	.long .Lap_setup - _ap_start
	.word KCS
_ap_stackpage:
	.long 0
_ap_cr3:
	.long 0x12345
	.code32
.Lap_setup:
	/* Setup segments. */
	mov $KDS, %ax
	mov %ax, %ss
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	/* Get AP configuration. */
	xor %ecx, %ecx
	mov %bx, %cx
	shl $4, %ecx
	mov (_ap_cr3 - _ap_start)(%ecx), %eax
	mov %eax, %cr3

	/* Check NX Support. */
	mov $0x80000001, %eax
	mov $0, %ecx
	cpuid
	and $(1 << 20), %edx
	jz .Lno_nx

	mov $MSR_IA32_EFER, %ecx
	rdmsr
	or $(1 << 11), %eax
	wrmsr
	/* NX should be enabled now. */

.Lno_nx:
	/* Setup CR registers. */
	mov %cr4, %eax
	or  $0x30, %eax
	mov  %eax, %cr4
	mov  %cr0, %eax
	or   $((1<<31)|(1<<16)), %eax
	mov  %eax, %cr0
	ljmp $0x08, $apsetup
ENTRY(_ap_end)

	.code32
apsetup:
1:
	/* Reload GDT and IDT */
	lgdt _gdtr
	lidt _idtr
	ljmp $KCS, $1f
1:
	/* Allocate and setup stack. */
	mov $pcpu_kstack, %esi
	mov $pcpu_kstackcnt, %edi
	mov (%edi), %eax
1:
	mov %eax, %ebx
	inc %ebx
	lock cmpxchg %ebx, (%edi)
	jz 1f
	pause
	jmp 1b
1:	
	lea (%esi, %eax, 8), %eax
	mov (%eax), %esp

	push %esp
	call _C_LABEL(i386_init_ap)
1:	hlt
	jmp 1b


print:
	movl $0x1000, %ecx
	movb $0x87, %ah
	movl $(_physmap_start + 0xb8000), %edi
1:	lodsb
	stosw
	cmp $0, %al
	loopne 1b
	ret

	.data
	.globl _bsp_cr3, _gdtr, _gdt, _idtr, _idt

_bsp_cr3:
	.long 0

	.align 64
	.globl _gdtr, _gdt
_gdtr:
	.hword (GDTSIZE * 8) - 1
	.long _gdt

	.align 64
_gdt:
	.zero GDTSIZE * 8

	
	.align 64
	.globl _idtr, _idt
_idtr:
	.word (256 * 8) - 1
	.long _idt
_idt:
	.skip  256 * 8

	.align 64
_tss:
	.skip 102
	.hword 104 // IOMAP over the edge.

_init_string:
	.asciz "_start_arg"
	
	.section .kstack_bsp, "aw"
	.align 4096
stackbtm:
	.skip 4096
	.globl _bsp_stacktop
_bsp_stacktop:
