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
