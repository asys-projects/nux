/*
  NUX: A kernel Library.
  Copyright (C) 2019 Gianluca Guida, glguida@tlbflush.org

  SPDX-License-Identifier:	BSD-2-Clause
*/

#include <inttypes.h>
#include <string.h>
#include <assert.h>

#include "internal.h"
#include "apic.h"

#include <nux/nux.h>
#include <nux/plt.h>

#define MAXCPUS 256

void *lapic_base = NULL;
unsigned lapics_no;
struct lapic_desc
{
  unsigned physid;
  unsigned platformid;
  uint32_t lint[2];
} lapics[MAXCPUS];


/*
 * Local APIC.
 */

#define APIC_DLVR_FIX   0
#define APIC_DLVR_PRIO  1
#define APIC_DLVR_SMI   2
#define APIC_DLVR_NMI   4
#define APIC_DLVR_INIT  5
#define APIC_DLVR_START 6
#define APIC_DLVR_EINT  7

/* LAPIC registers.  */
#define L_IDREG		0x20
#define L_VER		0x30
#define L_TSKPRIO	0x80
#define L_ARBPRIO	0x90
#define L_PROCPRIO	0xa0
#define L_EOI		0xb0
#define L_LOGDEST	0xd0
#define L_DESTFMT	0xe0
#define L_MISC		0xf0	/* Spurious vector */
#define L_ISR		0x100	/* 256 bit */
#define L_TMR		0x180	/* 256 bit */
#define L_IRR		0x200	/* 256 bit */
#define L_ERR		0x280
#define L_ICR_LO	0x300
#define L_ICR_HI	0x310
#define L_LVT_TIMER	0x320
#define L_LVT_THERM	0x330
#define L_LVT_PFMCNT	0x340
#define L_LVT_LINT(x)	(0x350 + (x * 0x10))
#define L_LVT_ERR	0x370
#define L_TMR_IC	0x380
#define L_TMR_CC	0x390
#define L_TMR_DIV	0x3e0

#define LAPIC_SIZE      (1UL << 12)

static uint32_t
lapic_read (unsigned reg)
{

  return *((volatile uint32_t *) (lapic_base + reg));
}

static void
lapic_write (unsigned reg, uint32_t data)
{

  *((volatile uint32_t *) (lapic_base + reg)) = data;
}

static unsigned
lapic_getcurrent (void)
{
  if (lapic_base == NULL)
    return 0;

  return (unsigned) (lapic_read (L_IDREG) >> 24);
}

unsigned
lapic_current (void)
{
  return lapic_getcurrent ();
}

static void
lapic_configure (void)
{
  unsigned i, physid = lapic_getcurrent ();
  struct lapic_desc *d = NULL;

  for (i = 0; i < lapics_no; i++)
    {
      if (lapics[i].physid == physid)
	d = lapics + i;
    }
  if (d == NULL)
    {
      warn ("Current CPU not in Platform Tables!");
      /* Try to continue, ignore the NMI configuration */
    }
  else
    {
      lapic_write (L_LVT_LINT (0), d->lint[0]);
      lapic_write (L_LVT_LINT (1), d->lint[1]);
    }
  /* Enable LAPIC */
  lapic_write (L_MISC, lapic_read (L_MISC) | 0x100);
}

static void
lapic_icr_write (uint32_t lo, uint32_t hi)
{
  while (lapic_read (L_ICR_LO) & (1 << 12))
    hal_cpu_relax ();

  lapic_write (L_ICR_HI, hi);
  lapic_write (L_ICR_LO, lo);
}

static void
lapic_ipi (unsigned physid, uint8_t dlvr, uint8_t vct)
{
  uint32_t hi, lo;

  lo = 0x4000 | (dlvr & 0x7) << 8 | vct;
  hi = (physid & 0xff) << 24;
  lapic_icr_write (lo, hi);
}

static void
lapic_ipi_broadcast (uint8_t dlvr, uint8_t vct)
{
  uint32_t lo;

  lo = (dlvr & 0x7) << 8 | vct | /*ALLBUTSELF*/ 0xc0000 | /*ASSERT*/ 0x4000;
  lapic_icr_write (lo, 0);
}

void
lapic_eoi (void)
{
  lapic_write (L_EOI, 0);
}

void
lapic_add_nmi (uint8_t pid, int l)
{
  int i;

  if (pid == 0xff)
    {
      for (i = 0; i < lapics_no; i++)
	lapics[i].lint[l] = (1L << 16) | (APIC_DLVR_NMI << 8);
      return;
    }
  for (i = 0; i < lapics_no; i++)
    {
      if (lapics[i].platformid == pid)
	{
	  if (l)
	    l = 1;
	  lapics[i].lint[l] = (APIC_DLVR_NMI << 8);
	  return;
	}
    }
  warn ("LAPIC NMI for non-existing platform ID %d", pid);
}

void
lapic_add (uint16_t physid, uint16_t plid)
{
  static unsigned i = 0;

  if (i < MAXCPUS)
    {
      lapics[i].physid = physid;
      lapics[i].platformid = plid;
      lapics[i].lint[0] = 0x10000;
      lapics[i].lint[1] = 0x10000;
      i++;
    }
  else
    {
      warn ("PCPU%d: MAXCPUS reached. Skipping", physid);
    }
}

void
lapic_init (uint64_t base, unsigned no)
{
  lapic_base = kva_physmap (base, LAPIC_SIZE, HAL_PTE_P | HAL_PTE_W);	/* XXX: trusting MTRR on
									 * caching. */
  lapics_no = no < MAXCPUS ? no : MAXCPUS;
  debug ("LAPIC PA: %08" PRIx64 " VA: %p", base, lapic_base);
}


/*
 * PCPU module: abstracted CPU operations.
 */

int
plt_pcpu_iterate (void)
{
  static int t = 0;

  if (t < lapics_no)
    return lapics[t++].physid;
  else
    {
      t = 0;
      return PLT_PCPU_INVALID;
    }
}

void
plt_pcpu_enter (void)
{
  lapic_configure ();
}

void
plt_pcpu_nmi (int pcpuid)
{
  lapic_ipi (pcpuid, APIC_DLVR_NMI, 0);
}

void
plt_pcpu_nmiall (void)
{
  lapic_ipi_broadcast (APIC_DLVR_NMI, 0);
}

void
plt_pcpu_ipi (int pcpuid)
{
  lapic_ipi (pcpuid, APIC_DLVR_FIX, APIC_VECT_IPIBASE);
}

void
plt_pcpu_ipiall (void)
{
  lapic_ipi_broadcast (APIC_DLVR_FIX, APIC_VECT_IPIBASE);
}

unsigned
plt_pcpu_id (void)
{
  return lapic_getcurrent ();
}

void
plt_pcpu_start (unsigned pcpuid, paddr_t start)
{
  if (pcpuid == lapic_getcurrent ())
    return;

  /* INIT-SIPI-SIPI sequence. */
  lapic_ipi (pcpuid, APIC_DLVR_INIT, 0);
  hw_delay ();
  lapic_ipi (pcpuid, APIC_DLVR_START, start >> 12);
  hw_delay ();
  lapic_ipi (pcpuid, APIC_DLVR_START, start >> 12);
  hw_delay ();
}
