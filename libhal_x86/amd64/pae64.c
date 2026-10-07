/*
  NUX: A kernel Library.
  Copyright (C) 2019 Gianluca Guida, glguida@tlbflush.org

  SPDX-License-Identifier:	BSD-2-Clause
*/

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <nux/nux.h>
#include <nux/types.h>
#include "../internal.h"

#define L1_SHIFT PAGE_SHIFT
#define L2_SHIFT (L1_SHIFT + 9)
#define L3_SHIFT (L2_SHIFT + 9)
#define L4_SHIFT (L3_SHIFT + 9)

#define ROUND_UP(_x, _s) ((_x) + ((1L << _s) - 1))

#define L4OFF(_va) (((_va) >> L4_SHIFT) & 0x1ff)
#define L3OFF(_va) (((_va) >> L3_SHIFT) & 0x1ff)
#define L2OFF(_va) (((_va) >> L2_SHIFT) & 0x1ff)
#define L1OFF(_va) (((_va) >> L1_SHIFT) & 0x1ff)

#define MKCANON(_va) ((int64_t)((_va) << 16) >> 16)
#define UNCANON(_va) ((_va) & ((1LL << 48) - 1))

/* The following RES definitions assume 48-bit MAX PA */
#define L4_RESPT 0x000F000000000080
#define L3_RESPT 0x000F000000000000
#define L3_RES1G 0x000F00003fffe000
#define L2_RESPT 0x000F000000000000
#define L2_RES2M 0x000F0000001fe000
#define L1_RESPT 0x000F000000000000

/* Assume PTE_P is set */
#define l4e_reserved(_pte) ((_pte) & L4_RESPT)
#define l3e_bigpage(_pte) ((_pte) & PTE_PS)
#define l3e_bigpage(_pte) ((_pte) & PTE_PS)
#define l3e_reserved(_pte) ((_pte) & (l3e_bigpage(_pte) ? L3_RES1G : L3_RESPT))
#define l2e_bigpage(_pte) ((_pte) & PTE_PS)
#define l2e_reserved(_pte) ((_pte) & (l2e_bigpage(_pte) ? L2_RES2M : L2_RESPT))
#define l1e_reserved(_pte) ((_pte) & L1_RESPT)

#define mkpte(_p, _f) (((uint64_t)(_p) << PAGE_SHIFT) | (_f))
#define pte_pfn(_p) ((pfn_t)(((uint64_t) (_p) & ~PTE_NX) >> PAGE_SHIFT))
#define pte_present(_pte) ((_pte) & PTE_P)

#define PTE_INVALID ((uint64_t)0)
#define PTEP_INVALID L1P_INVALID
#define mkptep_cur(_p) ((ptep_t)(uintptr_t)(_p))
#define mkptep_fgn(_p) ((ptep_t)(uintptr_t)(_p) | 1)
#define ptep_is_foreign(_p) ((_p) & 1)

extern int _linear_start;
static const pte_t *linaddr = (const pte_t *) &_linear_start;
static pte_t *linaddr_l2;
static pte_t *linaddr_l3;
static pte_t *linaddr_l4;

uint64_t
mkaddr (uint64_t l4off, uint64_t l3off, uint64_t l2off, uint64_t l1off)
{
  return
    MKCANON (((l4off << L4_SHIFT) | (l3off << L3_SHIFT) | (l2off << L2_SHIFT)
	      | (l1off << L1_SHIFT)));
}

pte_t
get_pte (ptep_t ptep)
{
  if (ptep_is_foreign (ptep))
    {
      pte_t *t, pte;
      pfn_t pfn;
      unsigned offset;

      pfn = ptep >> PAGE_SHIFT;
      offset = ptep >> 3 & 0x1ff;

      t = (pte_t *) pfn_get (pfn);
      pte = t[offset];
      pfn_put (pfn, t);

      return pte;
    }
  else
    {
      return *(pte_t *) (uintptr_t) ptep;
    }

}

pte_t
set_pte (ptep_t ptep, pte_t pte)
{
  pte_t old;

  if (ptep_is_foreign (ptep))
    {
      pte_t *t;
      pfn_t pfn;
      unsigned offset;

      pfn = ptep >> PAGE_SHIFT;
      offset = ptep >> 3 & 0x1ff;

      t = (pte_t *) pfn_get (pfn);
      old = t[offset];
      t[offset] = pte;
      pfn_put (pfn, t);
    }
  else
    {
      old = *(pte_t *) ptep;
      *(pte_t *) ptep = pte;
    }

  return old;
}

static pte_t
alloc_table (bool user)
{
  pfn_t pfn;

  pfn = pfn_alloc (0);
  if (pfn == PFN_INVALID)
    return PTE_INVALID;

  return mkpte (pfn, PTE_P | PTE_W | (user ? PTE_U : 0));
}

static ptep_t
linmap_get_l4p (unsigned long va)
{
  return mkptep_cur (linaddr_l4 + (UNCANON (va) >> L4_SHIFT));
}

static pte_t
linmap_get_l4e (unsigned long va)
{
  return *(pte_t *) (linaddr_l4 + (UNCANON (va) >> L4_SHIFT));
}

static ptep_t
linmap_get_l3p (unsigned long va, bool alloc, bool user)
{
  ptep_t l4p;
  pte_t l4e;

  l4p = linmap_get_l4p (va);
  l4e = get_pte (l4p);

  if (!pte_present (l4e))
    {
      if (!alloc)
	return PTEP_INVALID;
      l4e = alloc_table (user);
      if (l4e == PTE_INVALID)
	return PTEP_INVALID;
      set_pte (l4p, l4e);
      /* Not present, no TLB flush necessary. */
    }

  assert (!l4e_reserved (l4e) && "Invalid L4E.");

  return mkptep_cur (linaddr_l3 + (UNCANON (va) >> L3_SHIFT));
}

static ptep_t
linmap_get_l2p (unsigned long va, bool alloc, bool user)
{
  ptep_t l3p;
  pte_t l3e;

  l3p = linmap_get_l3p (va, alloc, user);
  if (l3p == PTEP_INVALID)
    return l3p;
  l3e = get_pte (l3p);

  if (!pte_present (l3e))
    {
      if (!alloc)
	return PTEP_INVALID;
      l3e = alloc_table (user);
      if (l3e == PTE_INVALID)
	return PTEP_INVALID;
      set_pte (l3p, l3e);
      /* Not present, no TLB flush necessary. */
    }

  assert (!l3e_reserved (l3e) && "Invalid L3E.");
  assert (!l3e_bigpage (l3e) && "Invalid page size.");

  return mkptep_cur (linaddr_l2 + (UNCANON (va) >> L2_SHIFT));
}

static ptep_t
linmap_get_l1p (unsigned long va, bool alloc, bool user)
{
  ptep_t l2p;
  pte_t l2e;

  l2p = linmap_get_l2p (va, alloc, user);
  if (l2p == PTEP_INVALID)
    return l2p;
  l2e = get_pte (l2p);

  if (!pte_present (l2e))
    {
      if (!alloc)
	return PTEP_INVALID;
      l2e = alloc_table (user);
      if (l2e == PTE_INVALID)
	return PTEP_INVALID;
      set_pte (l2p, l2e);
      /* Not present, no TLB flush necessary. */
    }

  assert (!l2e_reserved (l2e) && "Invalid L2E.");
  assert (!l2e_bigpage (l2e) && "Invalid page size.");

  return mkptep_cur (linaddr + (UNCANON (va) >> L1_SHIFT));
}

static ptep_t
get_umap_l4p (struct hal_umap *umap, unsigned long va)
{
  assert (L4OFF (va) < UMAP_L4PTES);

  if (umap == NULL)
    {
      /* Use LINMAP if current. */
      assert (L4OFF (va) < UMAP_L4PTES);
      return linmap_get_l4p (va);
    }

  return mkptep_cur (umap->l4 + L4OFF (va));
}


static pfn_t
get_umap_l3pfn (struct hal_umap *umap, unsigned long va, bool alloc)
{
  ptep_t l4p;
  pte_t l4e;

  l4p = get_umap_l4p (umap, va);
  l4e = get_pte (l4p);

  if (!pte_present (l4e))
    {
      if (!alloc)
	return PFN_INVALID;
      l4e = alloc_table (true /* user */ );
      if (l4e == PTE_INVALID)
	return PFN_INVALID;
      set_pte (l4p, l4e);
      /* Not present, no TLB flush necessary. */
    }

  assert (!l4e_reserved (l4e) && "Invalid L4E.");

  return pte_pfn (l4e);
}

static ptep_t
get_umap_l3p (struct hal_umap *umap, unsigned long va, bool alloc)
{
  pfn_t l3pfn;

  if (umap == NULL)
    {
      /* Use LINMAP if current. */
      assert (L4OFF (va) < UMAP_L4PTES);
      return linmap_get_l3p (va, alloc, true /* user */ );
    }

  l3pfn = get_umap_l3pfn (umap, va, alloc);
  if (l3pfn == PFN_INVALID)
    {
      return PTEP_INVALID;
    }

  return mkptep_fgn ((l3pfn << PAGE_SHIFT) + (L3OFF (va) << 3));
}

static pfn_t
get_umap_l2pfn (struct hal_umap *umap, unsigned long va, bool alloc)
{
  ptep_t l3p;
  pte_t l3e;

  l3p = get_umap_l3p (umap, va, alloc);
  if (l3p == PTEP_INVALID)
    return PFN_INVALID;
  l3e = get_pte (l3p);

  if (!pte_present (l3e))
    {
      if (!alloc)
	return PFN_INVALID;
      l3e = alloc_table (true /* user */ );
      if (l3e == PTE_INVALID)
	return PFN_INVALID;
      set_pte (l3p, l3e);
      /* Not present, no TLB flush necessary. */
    }

  assert (!l3e_reserved (l3e) && "Invalid L3E.");
  assert (!l3e_bigpage (l3e) && "Invalid page size.");

  return pte_pfn (l3e);
}

static ptep_t
get_umap_l2p (struct hal_umap *umap, unsigned long va, bool alloc)
{
  pfn_t l2pfn;

  if (umap == NULL)
    {
      /* Use LINMAP if current. */
      assert (L4OFF (va) < UMAP_L4PTES);
      return linmap_get_l2p (va, alloc, true /* user */ );
    }

  l2pfn = get_umap_l2pfn (umap, va, alloc);
  if (l2pfn == PFN_INVALID)
    {
      return PTEP_INVALID;
    }

  return mkptep_fgn ((l2pfn << PAGE_SHIFT) + (L2OFF (va) << 3));
}

static pfn_t
get_umap_l1pfn (struct hal_umap *umap, unsigned long va, bool alloc)
{
  ptep_t l2p;
  pte_t l2e;

  l2p = get_umap_l2p (umap, va, alloc);
  if (l2p == PTEP_INVALID)
    return PFN_INVALID;
  l2e = get_pte (l2p);

  if (!pte_present (l2e))
    {
      if (!alloc)
	return PFN_INVALID;
      l2e = alloc_table (true /* user */ );
      if (l2e == PTE_INVALID)
	return PFN_INVALID;
      set_pte (l2p, l2e);
      /* Not present, no TLB flush necessary. */
    }


  assert (!l2e_reserved (l2e) && "Invalid L2E.");
  assert (!l2e_bigpage (l2e) && "Invalid page size.");

  return pte_pfn (l2e);
}



ptep_t
umap_get_l1p (struct hal_umap *umap, unsigned long va, bool alloc)
{
  pfn_t l1pfn;

  if (umap == NULL)
    {
      /* Use LINMAP if current. */
      assert (L4OFF (va) < UMAP_L4PTES);
      return linmap_get_l1p (va, alloc, true /* user */ );
    }

  l1pfn = get_umap_l1pfn (umap, va, alloc);
  if (l1pfn == PFN_INVALID)
    {
      return PTEP_INVALID;
    }
  return mkptep_fgn ((l1pfn << PAGE_SHIFT) + (L1OFF (va) << 3));
}

unsigned long
pt_umap_minaddr (void)
{
  return 0;
}

unsigned long
pt_umap_maxaddr (void)
{
  return 1L << (39 + UMAP_LOG2_L4PTES);
}

/* Note: this does not check for va being user. */
hal_l1p_t
kmap_get_l1p (unsigned long va, bool alloc)
{
  return (hal_l1p_t) linmap_get_l1p (va, alloc, false /* !user */ );
}

void
pt_umap_debugwalk (struct hal_umap *umap, unsigned long va)
{
  unsigned i;
  pte_t pte;
  ptep_t ptep;

  i = 4;
  ptep = linmap_get_l4p (va);
  printf ("    L%d -", i);
  if (ptep == PTEP_INVALID)
    {
      printf (" <PTE PTR INVALID>\n\n");
      return;
    }
  pte = get_pte (ptep);
  printf (" <%lx>\n", pte);
  if (!pte_present(pte))
    {
      printf("\n");
      return;
    }

  i = 3;
  ptep = linmap_get_l3p (va, false, false);
  printf ("    L%d -", i);
  if (ptep == PTEP_INVALID)
    {
      printf (" <PTE PTR INVALID>\n\n");
      return;
    }
  pte = get_pte (ptep);
  printf (" <%lx>\n", pte);
  if (!pte_present(pte))
    {
      printf("\n");
      return;
    }

  i = 2;
  ptep = linmap_get_l2p (va, false, false);
  printf ("    L%d -", i);
  if (ptep == PTEP_INVALID)
    {
      printf (" <PTE PTR INVALID>\n\n");
      return;
    }
  pte = get_pte (ptep);
  printf (" <%lx>\n", pte);
  if (!pte_present(pte))
    {
      printf("\n");
      return;
    }

  i = 1;
  ptep = linmap_get_l1p (va, false, false);
  printf ("    L%d -", i);
  if (ptep == PTEP_INVALID)
    {
      printf (" <PTE PTR INVALID>\n\n");
      return;
    }
  pte = get_pte (ptep);
  printf (" <%lx>\n", pte);
  return;
}

void
hal_umap_init (struct hal_umap *umap)
{
  for (int i = 0; i < UMAP_L4PTES; i++)
    {
      umap->l4[i] = alloc_table (true);;
    }
}

void
hal_umap_bootstrap (struct hal_umap *umap)
{
  vaddr_t va = hal_virtmem_userbase ();
  int i;

  for (i = 0; i < UMAP_L4PTES; i++, va += (1L << L4_SHIFT))
    {
      ptep_t l4p;
      pte_t l4e;

      l4p = linmap_get_l4p (va);
      l4e = get_pte (l4p);

      if (!pte_present (l4e))
	{
	  l4e = alloc_table (true);
	  /* We're in bootstrap. Can assert. */
	  assert (l4e != PTE_INVALID);
	  set_pte (l4p, l4e);
	  /* Not present, no TLB flush necessary. */
	}
      umap->l4[i] = l4e;
    }

  /* Panic if the boot user mapping doesn't fit in a UMAP. */
  for (; i < 256; i++, va += (1L << L4_SHIFT))
    if (pte_present (linmap_get_l4e (va)))
      {
	halfatal ("Boot user mapping do not fit into UMAP");
      }
}

hal_tlbop_t
hal_umap_load (struct hal_umap *umap)
{
  vaddr_t va = hal_virtmem_userbase ();
  hal_tlbop_t tlbop = HAL_TLBOP_NONE;
  int i;

  for (i = 0; i < UMAP_L4PTES; i++, va += (1L << L4_SHIFT))
    {
      ptep_t l4p;
      pte_t oldl4e, newl4e;

      if (umap != NULL)
	newl4e = umap->l4[i];
      else
	newl4e = 0;

      l4p = linmap_get_l4p (va);
      oldl4e = set_pte (l4p, newl4e);
      tlbop |= hal_l1e_tlbop (oldl4e, newl4e);
    }
  return tlbop;
}

static bool
scan_l1 (pfn_t l1pfn, unsigned off, unsigned *l1off_out, hal_l1p_t * l1p_out,
	 hal_l1e_t * l1e_out)
{
  pte_t *l1ptr, l1e;

  l1ptr = pfn_get (l1pfn);
  for (unsigned i = off; i < 512; i++)
    {
      l1e = l1ptr[i];
      if (l1e != 0)
	{
	  if (l1p_out != NULL)
	    *l1p_out = mkptep_fgn ((l1pfn << PAGE_SHIFT) + (i << 3));
	  if (l1e_out != NULL)
	    *l1e_out = l1e;
	  *l1off_out = i;
	  pfn_put (l1pfn, l1ptr);
	  return true;
	}
    }
  pfn_put (l1pfn, l1ptr);
  return false;
}

static bool
scan_l2 (pfn_t l2pfn, unsigned off, unsigned *l2off_out, unsigned *l1off_out,
	 hal_l1p_t * l1p_out, hal_l1e_t * l1e_out)
{
  pte_t *l2ptr, l2e;
  pfn_t l1pfn;

  l2ptr = pfn_get (l2pfn);
  for (unsigned i = off; i < 512; i++)
    {
      l2e = l2ptr[i];
      if (pte_present (l2e))
	{
	  l1pfn = pte_pfn (l2e);
	  if (scan_l1 (l1pfn, 0, l1off_out, l1p_out, l1e_out))
	    {
	      *l2off_out = i;
	      pfn_put (l2pfn, l2ptr);
	      return true;
	    }
	}
    }
  pfn_put (l2pfn, l2ptr);
  return false;
}

static bool
scan_l3 (pfn_t l3pfn, unsigned off, unsigned *l3off_out, unsigned *l2off_out,
	 unsigned *l1off_out, hal_l1p_t * l1p_out, hal_l1e_t * l1e_out)
{
  pte_t *l3ptr, l3e;
  pfn_t l2pfn;

  l3ptr = pfn_get (l3pfn);
  for (unsigned i = off; i < 512; i++)
    {
      l3e = l3ptr[i];
      if (pte_present (l3e))
	{
	  l2pfn = pte_pfn (l3e);
	  if (scan_l2 (l2pfn, 0, l2off_out, l1off_out, l1p_out, l1e_out))
	    {
	      *l3off_out = i;
	      pfn_put (l3pfn, l3ptr);
	      return true;
	    }
	}
    }
  pfn_put (l3pfn, l3ptr);
  return false;
}

static bool
scan_l4 (struct hal_umap *umap, unsigned off, unsigned *l4off_out,
	 unsigned *l3off_out, unsigned *l2off_out, unsigned *l1off_out,
	 hal_l1p_t * l1p_out, hal_l1e_t * l1e_out)
{
  pte_t l4e;
  pfn_t l3pfn;
  for (unsigned i = off; i < UMAP_L4PTES; i++)
    {
      if (umap != NULL)
	l4e = umap->l4[i];
      else
	l4e = linmap_get_l4e (mkaddr (i, 0, 0, 0));

      if (pte_present (l4e))
	{
	  l3pfn = pte_pfn (l4e);
	  if (scan_l3
	      (l3pfn, 0, l3off_out, l2off_out, l1off_out, l1p_out, l1e_out))
	    {
	      *l4off_out = i;
	      return true;
	    }
	}
    }
  return false;
}

uaddr_t
pt_umap_next (struct hal_umap *umap, uaddr_t uaddr, hal_l1p_t * l1p_out,
	      hal_l1e_t * l1e_out)
{

  unsigned l4off = L4OFF (uaddr);
  unsigned l3off = L3OFF (uaddr);
  unsigned l2off = L2OFF (uaddr);
  unsigned l1off = L1OFF (uaddr);
  pfn_t l1pfn, l2pfn, l3pfn;
  unsigned l4next, l3next, l2next, l1next;

  /* Check till end of current l1. */
  l1pfn = get_umap_l1pfn (umap, uaddr, false);
  if (l1pfn != PFN_INVALID
      && scan_l1 (l1pfn, l1off + 1, &l1next, l1p_out, l1e_out))
    {
      return mkaddr (l4off, l3off, l2off, l1next);
    }

  /* Check till end of current l2. */
  l2pfn = get_umap_l2pfn (umap, uaddr, false);
  if (l2pfn != PFN_INVALID
      && scan_l2 (l2pfn, l2off + 1, &l2next, &l1next, l1p_out, l1e_out))
    {
      return mkaddr (l4off, l3off, l2next, l1next);
    }

  /* Check till end of current l3. */
  l3pfn = get_umap_l3pfn (umap, uaddr, false);
  if (l3pfn != PFN_INVALID
      && scan_l3 (l3pfn, l3off + 1, &l3next, &l2next, &l1next, l1p_out,
		  l1e_out))
    {
      return mkaddr (l4off, l3next, l2next, l1next);
    }

  /* Scan L4 until end of UMAP area. */
  if (scan_l4
      (umap, l4off + 1, &l4next, &l3next, &l2next, &l1next, l1p_out, l1e_out))
    {
      return mkaddr (l4next, l3next, l2next, l1next);
    }

  return UADDR_INVALID;
}

void
pt_umap_free (struct hal_umap *umap)
{
  pte_t l4e;
  pfn_t l3pfn;
  pte_t *l3ptr, l3e;
  pfn_t l2pfn;
  pte_t *l2ptr, l2e;
  pfn_t l1pfn;

  for (unsigned i = 0; i < UMAP_L4PTES; i++)
    {
      l4e = umap->l4[i];
      if (pte_present (l4e))
	{
	  l3pfn = pte_pfn (l4e);
	  l3ptr = pfn_get (l3pfn);
	  for (unsigned i = 0; i < 512; i++)
	    {
	      l3e = l3ptr[i];
	      if (pte_present (l3e))
		{
		  l2pfn = pte_pfn (l3e);
		  l2ptr = pfn_get (l2pfn);
		  for (unsigned i = 0; i < 512; i++)
		    {
		      l2e = l2ptr[i];
		      if (pte_present (l2e))
			{
			  l1pfn = pte_pfn (l2e);
			  pfn_free (l1pfn);
			}
		    }
		  pfn_put (l2pfn, l2ptr);
		  pfn_free (l2pfn);
		}
	    }
	  pfn_put (l3pfn, l3ptr);
	  pfn_free (l3pfn);
	}
      umap->l4[i] = PTE_INVALID;
    }
}

void
pae64_init_ap (void)
{
  unsigned long linoff = L4OFF ((unsigned long) linaddr);
  pte_t *va, *cr3va;
  pfn_t pfn, cr3pfn;


  pfn = pfn_alloc (0);
  assert (pfn != PFN_INVALID);
  cr3pfn = btop (read_cr3 ());

  /* Copy the kernel mappings. */
  va = kva_physmap (ptob (pfn), PAGE_SIZE, HAL_PTE_W | HAL_PTE_P);
  cr3va = kva_physmap (ptob (cr3pfn), PAGE_SIZE, HAL_PTE_P);
  memcpy (va, cr3va, PAGE_SIZE);
  va[linoff] = mkpte (pfn, PTE_P | PTE_W);	/* Point the linaddr back at itself. */
  kva_unmap (va, PAGE_SIZE / 2);
  kva_unmap (cr3va, PAGE_SIZE / 2);

  write_cr3 (ptob (pfn));
}

void
pae64_init (void)
{
  unsigned long linoff = L4OFF ((unsigned long) linaddr);
  linaddr_l2 = (pte_t *) mkaddr (linoff, linoff, 0, 0);
  linaddr_l3 = (pte_t *) mkaddr (linoff, linoff, linoff, 0);
  linaddr_l4 = (pte_t *) mkaddr (linoff, linoff, linoff, linoff);
}
