/*
  NUX: A kernel Library.
  Copyright (C) 2019 Gianluca Guida, glguida@tlbflush.org

  SPDX-License-Identifier:	BSD-2-Clause
*/

/*
  Platform power: ACPI soft-off (S5) and reset.

  The registers come from the FADT (acpi_fadt_scan): the PM1 control
  blocks, the S5 sleep types found in the DSDT, and the reset register.
  When ACPI cannot do it, the legacy PC means are tried: the keyboard
  controller's reset line and the PCI reset control register (0xcf9),
  and the soft-off ports of the common emulators.
*/

#include <nux/hal.h>
#include <nux/nux.h>
#include <nux/plt.h>

#include "acpitbl.h"
#include "internal.h"

#define PM1_SLP_TYP_SHIFT 10
#define PM1_SLP_EN (1 << 13)

static uint32_t pm1a_cnt, pm1b_cnt;
static uint16_t slp_typa, slp_typb;
static bool s5_valid;

static bool reset_valid;
static uint8_t reset_space, reset_value;
static uint64_t reset_addr;

void
acpi_power_pm1 (uint32_t pm1a, uint32_t pm1b)
{
  pm1a_cnt = pm1a;
  pm1b_cnt = pm1b;
}

void
acpi_power_s5 (uint16_t typa, uint16_t typb)
{
  slp_typa = typa;
  slp_typb = typb;
  s5_valid = true;
}

void
acpi_power_resetreg (uint8_t space, uint64_t addr, uint8_t value)
{
  if (space != ACPI_GAS_IO && space != ACPI_GAS_MEMORY)
    return;
  reset_space = space;
  reset_addr = addr;
  reset_value = value;
  reset_valid = true;
}

void
acpi_power_info (void)
{
  info ("ACPI power: PM1a_CNT %" PRIx32 ", S5 %s (%x), reset %s",
	pm1a_cnt, s5_valid ? "found" : "not found", slp_typa,
	reset_valid ? "register" : "legacy");
}

void
plt_poweroff (void)
{
  if (s5_valid && pm1a_cnt)
    {
      hal_cpu_out (2, pm1a_cnt,
		   (slp_typa << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN);
      if (pm1b_cnt)
	hal_cpu_out (2, pm1b_cnt,
		     (slp_typb << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN);
    }
  /* Emulators: QEMU's PIIX4 PM (0x604) and Bochs/old QEMU (0xb004). */
  hal_cpu_out (2, 0x604, 0x2000);
  hal_cpu_out (2, 0xb004, 0x2000);
}

void
plt_reset (void)
{
  if (reset_valid && reset_space == ACPI_GAS_IO)
    hal_cpu_out (1, reset_addr, reset_value);
  else if (reset_valid && reset_space == ACPI_GAS_MEMORY)
    {
      volatile uint8_t *r = kva_physmap (reset_addr, 1, HAL_PTE_P | HAL_PTE_W);
      *r = reset_value;
      kva_unmap ((void *) r, 1);
    }
  /* Keyboard controller: pulse the reset line. */
  hal_cpu_out (1, 0x64, 0xfe);
  /* PCI reset control register: hard reset. */
  hal_cpu_out (1, 0xcf9, 0x06);
}
