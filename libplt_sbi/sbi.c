#include <nux/plt.h>
#include <nux/nmiemul.h>
#include <nux/apxh.h>
#include <nux/hal.h>
#include <nux/nux.h>
#include <libfdt.h>
#include <string.h>

static uint64_t timebase_frequency = 0;

#if 0
static struct plt_cpu
{
  unsigned plic_ctx;		/* S-mode PLIC context. */
} pltcpus[HAL_MAXCPUS];
#endif

static void
_get_cells (const void *fdt, int noff, unsigned *addr, unsigned *size)
{
  unsigned a, s;
  int len;
  const void *prop;

  /* Initialise to spec default. */
  a = 2, s = 1;

  noff = fdt_parent_offset (fdt, noff);

  prop = fdt_getprop (fdt, noff, "#address-cells", &len);
  if (prop && len == sizeof (uint32_t))
    {
      a = fdt32_to_cpu (*(uint32_t *) prop);
    }
  else
    {
      warn ("DT: warning: using default #address-cells %d for node %s\n", a,
	    fdt_get_name (fdt, noff, NULL));
    }

  prop = fdt_getprop (fdt, noff, "#size-cells", &len);
  if (prop && len == sizeof (uint32_t))
    {
      s = fdt32_to_cpu (*(uint32_t *) prop);
    }
  else
    {
      warn ("DT: warning: using default #size-cells %d for node %s\n", s,
	    fdt_get_name (fdt, noff, NULL));
    }

  *addr = a;
  *size = s;
}

static bool
_get_reg (const void *fdt, int noff, unsigned idx, uint64_t * base,
	  uint64_t * length)
{
  int len;
  const void *prop;
  unsigned addrsz, sizesz, regsz;
  uint64_t b, l;

  _get_cells (fdt, noff, &addrsz, &sizesz);
  regsz = addrsz + sizesz;;

  prop = fdt_getprop (fdt, noff, "reg", &len);
  if (!prop)
    return false;

  if (len < (idx + 1) * regsz * sizeof (uint32_t))
    return false;

  prop += idx * regsz * sizeof (uint32_t);

  b = 0;
  for (unsigned i = 0; i < addrsz; i++)
    {
      b = (b << 32) | fdt32_to_cpu (*(uint32_t *) prop);
      prop += sizeof (uint32_t);
    }

  l = 0;
  for (unsigned i = 0; i < sizesz; i++)
    {
      l = (l << 32) | fdt32_to_cpu (*(uint32_t *) prop);
      prop += sizeof (uint32_t);
    }

  if (base)
    *base = b;
  if (length)
    *length = l;
  return true;
}

static void
plic_init (const void *fdt, int noff)
{
  int len;
  const void *prop;
  uint64_t base, length;

  if (!_get_reg (fdt, noff, 0, &base, &length))
    return;
  printf ("PLIC: %s [%016" PRIx64 ":%016" PRIx64 "]\n",
	  fdt_get_name (fdt, noff, NULL), base, base + length);

  prop = fdt_getprop (fdt, noff, "interrupts-extended", &len);

  printf ("PLIC: External Interrupts Contexts: ");
  for (int i = 0; i < len; i += sizeof (uint32_t) * 2)
    {
      uint32_t phandle, intr;
      phandle = fdt32_to_cpu (*(uint32_t *) (prop + i));
      intr = fdt32_to_cpu (*((uint32_t *) (prop + i) + 1));

      /*
       * External Interrupts for S-mode. The bit we're interested
       * about.
       */
      if (intr == 9)
	{
	  uint64_t cpu;
	  int hoff, poff;

	  hoff = fdt_node_offset_by_phandle (fdt, phandle);
	  if (hoff < 0)
	    continue;
	  poff = fdt_parent_offset (fdt, hoff);
	  if (poff < 0)
	    continue;
	  if (!_get_reg (fdt, poff, 0, &cpu, NULL))
	    continue;

	  printf ("%" PRId64 "[%d] ", cpu, i / (sizeof (uint32_t) * 2));
	}
    }
  printf ("\n");
}

void
plt_init (void)
{
  const struct apxh_pltdesc *desc;
  struct fdt_header *fdth;
  const void *fdt, *prop;
  int len, cpus_off;
  uint32_t size;

  desc = hal_pltinfo ();
  if (desc == NULL)
    fatal ("Invalid PLT Boot Table.");

  if (desc->type != PLT_DTB)
    fatal ("No Device Tree Found.");

  info ("DT: DTB at %016" PRIx64, desc->pltptr);

  fdth =
    (struct fdt_header *) kva_physmap (desc->pltptr, sizeof (*fdth),
				       HAL_PTE_P);
  if (fdt_check_header (fdth) != 0)
    fatal ("Invalid DTB Header.");

  size = fdt32_to_cpu (fdth->totalsize);

  kva_unmap (fdth, sizeof (*fdth));

  fdt = (const void *) kva_physmap (desc->pltptr, size, HAL_PTE_P);

  /*
   * Scan the /cpus node, gathering information about HARTs, timer and
   * interrupts.
   */
  cpus_off = fdt_path_offset (fdt, "/cpus");
  if (cpus_off < 0)
    {
      fatal ("Device tree does not contain '/cpus' node.");
    }

  /*
   * Technically the Device Tree specification says that
   * 'timebase-frequency' should be a property of a single CPU node.
   * Practically in RV this is often found in /cpus.
   */
  prop = fdt_getprop (fdt, cpus_off, "timebase-frequency", &len);
  if (prop != NULL)
    {
      if (len == sizeof (uint32_t))
	{
	  timebase_frequency = fdt32_to_cpu (*(uint32_t *) prop);
	}
      else if (len == sizeof (uint64_t))
	{
	  timebase_frequency = fdt32_to_cpu (*(uint64_t *) prop);
	}
      else
	{
	  warn ("Unexpected length %d in %s/timebase-frequency\n", len,
		fdt_get_name (fdt, cpus_off, NULL));
	}
    }

  printf ("DT: ");

  for (int _cpu_off = fdt_first_subnode (fdt, cpus_off);
       _cpu_off >= 0; _cpu_off = fdt_next_subnode (fdt, _cpu_off))
    {
      const char *name;
      name = fdt_get_name (fdt, _cpu_off, NULL);
      if (name == NULL)
	continue;

      if (strncmp (name, "cpu@", 4) != 0)
	continue;

      printf ("%s ", name);

      prop = fdt_getprop (fdt, cpus_off, "timebase-frequency", &len);
      if (prop != NULL)
	{
	  uint64_t freq;
	  if (len == sizeof (uint32_t))
	    {
	      freq = fdt32_to_cpu (*(uint32_t *) prop);
	    }
	  else if (len == sizeof (uint64_t))
	    {
	      freq = fdt32_to_cpu (*(uint64_t *) prop);
	    }
	  else
	    continue;		/* Let's ignore an invalid
				 * value. Should be fatal? */

	  if (timebase_frequency == 0)
	    timebase_frequency = freq;
	  else if (timebase_frequency != freq)
	    fatal
	      ("Inconsistent timebase frequencies: previous %lx, found %lx\n",
	       timebase_frequency, freq);
	}
    }

  printf ("\n");

  printf ("DT: timebase-frequency: %ld\n", timebase_frequency);


  /* Search for PLIC. */
  for (int noff = fdt_next_node (fdt, -1, NULL);
       noff >= 0; noff = fdt_next_node (fdt, noff, NULL))
    {
      int pos = 0;
      prop = fdt_getprop (fdt, noff, "compatible", &len);
      if (prop)
	{
	  while (pos < len)
	    {
	      if (!strncmp ((char *) prop + pos, "sifive,plic-1.0.0", 17))
		{
		  plic_init (fdt, noff);
		}
	      pos += strlen ((char *) prop + pos) + 1;
	    }
	}
    }

  kva_unmap ((void *) fdt, size);
}

void
plt_pcpu_enter (void)
{
  /* TODO */
}

int
plt_pcpu_iterate (void)
{
  static int next_pcpu = 0;

  /* TODO */

  if (next_pcpu++ == 0)
    return 0;
  else
    return PLT_PCPU_INVALID;
}

void
riscv_ipi (unsigned long mask)
{
  asm volatile ("mv a0, %0\n"
		"li a7, 4\n" "ecall\n"::"r" (&mask):"a0", "a1", "a7");
}

void
plt_pcpu_ipiall (void)
{
  nmiemul_ipi_setall ();
  asm volatile ("csrsi sip, 2\n");
  riscv_ipi (-1);
}

void
plt_pcpu_ipi (int cpu)
{
  nmiemul_ipi_set (cpu);
  if (cpu == cpu_id ())
    asm volatile ("csrsi sip, 2\n");
  else
  riscv_ipi (1L << cpu);
}

void
plt_pcpu_nmiall (void)
{
  nmiemul_nmi_setall ();
  asm volatile ("csrsi sip, 2\n");
  riscv_ipi (-1);
}

void
plt_pcpu_nmi (int cpu)
{
  nmiemul_nmi_set (cpu);
  if (cpu == cpu_id ())
    asm volatile ("csrsi sip, 2\n");
  else
  riscv_ipi (1L << cpu);
}

void
plt_pcpu_start (unsigned cpu, unsigned long startaddr)
{
  /* TODO */
}

unsigned
plt_pcpu_id (void)
{
  return 0;
}

bool
plt_vect_process (unsigned vect)
{
  /* TODO */
  return false;
}

enum plt_irq_type
plt_irq_type (unsigned irq)
{
  /* TODO */
  return PLT_IRQ_INVALID;
}

void
plt_irq_enable (unsigned irq)
{
  /* TODO */
}

void
plt_irq_disable (unsigned irq)
{
  /* TODO */
}

unsigned
plt_irq_max (void)
{
  /* TODO */
  return 0;
}

bool
plt_msi_alloc (unsigned *irq, uint64_t * addr, uint32_t * data)
{
  /* TODO: the IMSIC (AIA). */
  return false;
}

/*
  SBI System Reset extension (SRST): reset type 0 is shutdown, 1 cold
  reboot.  Without it, the legacy SBI shutdown call.
*/
#define SBI_EXT_SRST 0x53525354
#define SBI_SRST_SHUTDOWN 0
#define SBI_SRST_COLD_REBOOT 1
#define SBI_EXT_LEGACY_SHUTDOWN 0x08

static void
sbi_system_reset (unsigned long type)
{
  register unsigned long a0 asm ("a0") = type;
  register unsigned long a1 asm ("a1") = 0;	/* No reason. */
  register unsigned long a6 asm ("a6") = 0;	/* FID 0: system reset. */
  register unsigned long a7 asm ("a7") = SBI_EXT_SRST;

  asm volatile ("ecall":"+r" (a0), "+r" (a1):"r" (a6), "r" (a7):"memory");
}

void
plt_poweroff (void)
{
  sbi_system_reset (SBI_SRST_SHUTDOWN);
  asm volatile ("li a7, %0\n" "ecall\n"::"i" (SBI_EXT_LEGACY_SHUTDOWN):"a0",
		"a7");
}

void
plt_reset (void)
{
  sbi_system_reset (SBI_SRST_COLD_REBOOT);
}

void
plt_eoi_ipi (void)
{
  /* Nothing. */
}

void
plt_eoi_irq (unsigned irq)
{
  /* TODO. */
}

void
plt_eoi_timer (void)
{
  /* Nothing. */
}

struct hal_frame *
plt_interrupt (unsigned vect, struct hal_frame *f)
{
  struct hal_frame *r;

  switch (vect)
    {
    case 1:			/* Supervisor Software Interrupt. */
      r = nmiemul_entry (f);
      break;

    case 5:			/* Supervisor Timer Interrupt. */
      plt_tmr_clralm ();
      r = hal_entry_timer (f);
      break;

    case 9:			/* Supervisor External Interrupt. */
      /* TODO: External interrupts. */
      r = f;
      break;

    default:
      r = f;
    }

  return r;
}

uint64_t tmr_offset = 0;

uint64_t
plt_tmr_ctr (void)
{
  uint64_t time;

  asm volatile ("rdtime %0\n":"=r" (time));
  return time + tmr_offset;
}

void
plt_tmr_setctr (uint64_t alm)
{
  uint64_t time;

  asm volatile ("rdtime %0\n":"=r" (time));
  tmr_offset = alm - time;
}

void
plt_tmr_setalm (uint64_t alm)
{
  alm += plt_tmr_ctr ();
  asm volatile ("mv a0, %0\n"
		"mv a6, x0\n"
		"mv a7, x0\n" "ecall\n"::"r" (alm):"a0", "a6", "a7");
}


uint64_t
plt_tmr_period (void)
{
  return 1000000000000000L / timebase_frequency;
}

void
plt_tmr_clralm (void)
{
  asm volatile ("li a0, -1\n"
		"mv a6, x0\n" "mv a7, x0\n" "ecall\n":::"a0", "a6", "a7");
}
