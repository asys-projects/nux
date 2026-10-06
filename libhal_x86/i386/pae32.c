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
#define L2_SHIFT (9 + PAGE_SHIFT)
#define L3_SHIFT (9 + 9 + PAGE_SHIFT)

#define L3OFF(_va) (((_va) >> L3_SHIFT) & 3)
#define L2OFF(_va) (((_va) >> L2_SHIFT) & 0x1ff)
#define L1OFF(_va) (((_va) >> L1_SHIFT) & 0x1ff)

/* The following RES definitions assume 48-bit MAX PA */
#define L2_RESPT 0x7FFF000000000000LL
#define L2_RES2M 0x7FFF0000001FE000LL
#define L1_RESPT 0x7FFF000000000000LL

#define l2e_bigpage(_pte) ((_pte) & PTE_PS)
#define l2e_reserved(_pte) ((_pte) & (l2e_bigpage(_pte) ? L2_RES2M : L2_RESPT))
#define l1e_reserved(_pte) ((_pte) & L1_RESPT)

#define mkpte(_p, _f) (((uint64_t)(_p) << PAGE_SHIFT) | (_f))
#define pte_pfn(_p) ((_p & ~PTE_NX) >> PAGE_SHIFT)
#define pte_present(_pte) ((_pte) & PTE_P)

#define PTE_INVALID ((uint64_t)0)
#define PTEP_INVALID L1P_INVALID
#define mkptep_cur(_p) ((ptep_t)(uintptr_t)(_p))
#define mkptep_fgn(_p) ((ptep_t)(_p) | 1)
#define ptep_is_foreign(_p) ((_p) & 1)

extern int _linear_start;
extern int _linear_l2table;
extern int _linear_l3table;
const vaddr_t linaddr = (vaddr_t) & _linear_start;
const vaddr_t l2_linaddr = (vaddr_t) & _linear_l2table;
pte_t *l3_linaddr = (pte_t *) & _linear_l3table;

uint64_t
mkaddr (uint64_t l3off, uint64_t l2off, uint64_t l1off)
{
  return (l3off << L3_SHIFT) | (l2off << L2_SHIFT) | (l1off << L1_SHIFT);
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
      old = *(pte_t *) (uintptr_t) ptep;
      *(pte_t *) (uintptr_t) ptep = pte;
    }

  return old;
}

static pte_t
alloc_l3table ()
{
  pfn_t pfn;

  pfn = pfn_alloc (0);
  if (pfn == PFN_INVALID)
    return PTE_INVALID;

  return mkpte (pfn, PTE_P);
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
linmap_get_l3p (unsigned long va)
{
  return mkptep_cur (l3_linaddr + L3OFF (va));
}

static ptep_t
linmap_get_l2p (unsigned long va, bool alloc, bool user)
{
  ptep_t l3p;
  pte_t l3e;

  l3p = linmap_get_l3p (va);
  l3e = get_pte (l3p);

  if (!pte_present (l3e))
    {
      if (!alloc)
	return PTEP_INVALID;
      l3e = alloc_l3table ();
      if (l3e == PTE_INVALID)
	return PTEP_INVALID;
      set_pte (l3p, l3e);
      /* Not present, no TLB flush necessary. */
    }

  va &= ~((1L << L2_SHIFT) - 1);
  return mkptep_cur (l2_linaddr + (va >> 18));
}

static ptep_t
linmap_get_l1p (unsigned long va, bool alloc, bool user)
{
  ptep_t l2p;
  pte_t l2e;

  l2p = linmap_get_l2p (va, alloc, user);
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

  va &= ~((1L << L1_SHIFT) - 1);
  return mkptep_cur (linaddr + (va >> 9));
}

static ptep_t
get_umap_l3p (struct hal_umap *umap, unsigned long va)
{
  if (umap == NULL)
    {
      /* Use PMAP if current. */
      assert (L3OFF (va) < UMAP_L3PTES);
      return linmap_get_l3p (va);
    }

  assert (L3OFF (va) < UMAP_L3PTES);

  return mkptep_cur (umap->l3 + L3OFF (va));
}

static pfn_t
get_umap_l2pfn (struct hal_umap *umap, unsigned long va, bool alloc)
{
  ptep_t l3p;
  pte_t l3e;

  l3p = get_umap_l3p (umap, va);
  l3e = get_pte (l3p);

  if (!pte_present (l3e))
    {
      if (!alloc)
	return PFN_INVALID;
      l3e = alloc_l3table ();
      if (l3e == PTE_INVALID)
	return PFN_INVALID;
      set_pte (l3p, l3e);
      /* Not present, no TLB flush necessary. */
    }

  return pte_pfn (l3e);
}

static ptep_t
get_umap_l2p (struct hal_umap *umap, unsigned long va, bool alloc)
{
  pfn_t l2pfn;

  if (umap == NULL)
    {
      /* Use PMAP if current. */
      assert (L3OFF (va) < UMAP_L3PTES);
      return linmap_get_l2p (va, alloc, true /* user */ );
    }

  l2pfn = get_umap_l2pfn (umap, va, alloc);
  if (l2pfn == PFN_INVALID)
    {
      return PTEP_INVALID;
    }

  return mkptep_fgn (((ptep_t) l2pfn << PAGE_SHIFT) + (L2OFF (va) << 3));
}

static pfn_t
get_umap_l1pfn (struct hal_umap *umap, unsigned long va, bool alloc)
{
  ptep_t l2p;
  pte_t l2e;

  l2p = get_umap_l2p (umap, va, alloc);
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
      /* Use PMAP if current. */
      assert (L3OFF (va) < UMAP_L3PTES);
      return linmap_get_l1p (va, alloc, true /* user */ );
    }

  l1pfn = get_umap_l1pfn (umap, va, alloc);
  if (l1pfn == PFN_INVALID)
    {
      return PTEP_INVALID;
    }
  return mkptep_fgn (((ptep_t) l1pfn << PAGE_SHIFT) + (L1OFF (va) << 3));
}

unsigned long
pt_umap_minaddr (void)
{
  return 0;
}

unsigned long
pt_umap_maxaddr (void)
{
  return 3L << L3_SHIFT;
}

/* Note: this does not check for va being user. */
hal_l1p_t
kmap_get_l1p (unsigned long va, bool alloc)
{
  return (hal_l1p_t) linmap_get_l1p (va, alloc, false /* !user */ );
}

void
hal_umap_init (struct hal_umap *umap)
{
  /*
     With PAE the CPU loads the L3 entries (PDPTEs) when CR3 is loaded,
     and the linear mappings only mirror them: an L3 entry cannot be
     added to a UMAP while it is loaded in a CPU.  Allocate all the
     user L2 tables upfront, as hal_umap_bootstrap() does.
   */
  for (int i = 0; i < UMAP_L3PTES; i++)
    {
      umap->l3[i] = alloc_l3table ();
    }
}

void
hal_umap_bootstrap (struct hal_umap *umap)
{
  vaddr_t va = hal_virtmem_userbase ();
  int i;

  for (i = 0; i < UMAP_L3PTES; i++, va += (1L << L3_SHIFT))
    {
      ptep_t l3p;
      pte_t l3e;

      l3p = linmap_get_l3p (va);
      l3e = get_pte (l3p);

      if (!pte_present (l3e))
	{
	  l3e = alloc_l3table ();
	  /* We're in bootstrap. Can assert. */
	  assert (l3e != PTE_INVALID);
	  set_pte (l3p, l3e);
	  /* Not present, no TLB flush necessary. */
	}
      umap->l3[i] = l3e & 0x0000fffffffff001LL;	/* Remove flags from PTE. */
    }
}

hal_tlbop_t
hal_umap_load (struct hal_umap *umap)
{
  unsigned long linoff = ((unsigned long) l3_linaddr & PAGE_MASK) >> 3;
  hal_tlbop_t tlbop = HAL_TLBOP_NONE;
  pfn_t cr3pfn, kpdpte_pfn;
  pte_t *pdptes, kpdpte, *kpd;

  /* Unfortunately, in PAE the linear mappings do not point to the
     root. */
  cr3pfn = read_cr3 () >> PAGE_SHIFT;
  pdptes = (pte_t *) pfn_get (cr3pfn);
  for (int i = 0; i < UMAP_L3PTES; i++)
    pdptes[i] = umap == NULL ? 0 : umap->l3[i];
  kpdpte = pdptes[3];
  pfn_put (cr3pfn, pdptes);

  hal_l1e_unbox (kpdpte, &kpdpte_pfn, NULL);
  assert (kpdpte_pfn != PFN_INVALID);
  kpd = (pte_t *) pfn_get (kpdpte_pfn);
  for (int i = 0; i < UMAP_L3PTES; i++)
    {
      kpd[linoff + i] = umap == NULL ? 0 : umap->l3[i] | PTE_W;
    }
  kpd[linoff + 3] = kpdpte | PTE_W;
  pfn_put (kpdpte_pfn, kpd);

  tlbop |= HAL_TLBOP_FLUSH;

  return tlbop;
}

void
pt_umap_debugwalk (struct hal_umap *umap, unsigned long va)
{
  unsigned i;
  pte_t pte;
  ptep_t ptep;

  i = 3;
  ptep = linmap_get_l3p (va);
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
	    *l1p_out = mkptep_fgn (((ptep_t) l1pfn << PAGE_SHIFT) + (i << 3));
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
scan_l3 (struct hal_umap *umap, unsigned off,
	 unsigned *l3off_out, unsigned *l2off_out, unsigned *l1off_out,
	 hal_l1p_t * l1p_out, hal_l1e_t * l1e_out)
{
  pte_t l3e;
  pfn_t l2pfn;
  for (unsigned i = off; i < UMAP_L3PTES; i++)
    {
      if (umap == NULL)
	{
	  ptep_t l3p;

	  l3p = linmap_get_l3p (mkaddr (i, 0, 0));
	  l3e = get_pte (l3p);
	}
      else
	l3e = umap->l3[i];

      if (pte_present (l3e))
	{
	  l2pfn = pte_pfn (l3e);
	  if (scan_l2 (l2pfn, 0, l2off_out, l1off_out, l1p_out, l1e_out))
	    {
	      *l3off_out = i;
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

  unsigned l3off = L3OFF (uaddr);
  unsigned l2off = L2OFF (uaddr);
  unsigned l1off = L1OFF (uaddr);
  pfn_t l1pfn, l2pfn;
  unsigned l3next, l2next, l1next;

  /* Check till end of current l1. */
  l1pfn = get_umap_l1pfn (umap, uaddr, false);
  if (l1pfn != PFN_INVALID
      && scan_l1 (l1pfn, l1off + 1, &l1next, l1p_out, l1e_out))
    {
      return mkaddr (l3off, l2off, l1next);
    }

  /* Check till end of current l2. */
  l2pfn = get_umap_l2pfn (umap, uaddr, false);
  if (l2pfn != PFN_INVALID
      && scan_l2 (l2pfn, l2off + 1, &l2next, &l1next, l1p_out, l1e_out))
    {
      return mkaddr (l3off, l2next, l1next);
    }

  if (scan_l3 (umap, l3off + 1, &l3next, &l2next, &l1next, l1p_out, l1e_out))
    {
      return mkaddr (l3next, l2next, l1next);
    }

  return UADDR_INVALID;
}

void
pt_umap_free (struct hal_umap *umap)
{
  pfn_t l2pfn, l1pfn;
  pte_t l3e, *l2ptr, l2e;

  for (unsigned i = 0; i < UMAP_L3PTES; i++)
    {
      l3e = umap->l3[i];
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
	  /* Release the PFN cache slot before freeing the page: a slot
	     leaked here per L2 table exhausts the cache after a few
	     dozen address spaces have been destroyed. */
	  pfn_put (l2pfn, l2ptr);
	  pfn_free (l2pfn);
	}
      umap->l3[i] = PTE_INVALID;
    }
}

void
pae32_init_ap (void)
{
  unsigned long linoff = ((unsigned long) l3_linaddr & PAGE_MASK) >> 3;
  pte_t *va;
  pfn_t l2pfn[4], l3pfn;

  for (int i = 0; i < 4; i++)
    {
      l2pfn[i] = pfn_alloc (0);
      assert (l2pfn[i] != PFN_INVALID);
    }

  for (int i = 0; i < 4; i++)
    {
      va = kva_physmap (ptob (l2pfn[i]), PAGE_SIZE, HAL_PTE_W | HAL_PTE_P);
      memcpy (va, (void *) (l2_linaddr + ((uint32_t) i << 30 >> 18)),
	      PAGE_SIZE);
      kva_unmap (va, PAGE_SIZE);
    }

  /* Third l2 is special, has kernel and linear mappings. */
  va = kva_physmap (ptob (l2pfn[2]), PAGE_SIZE, HAL_PTE_W | HAL_PTE_P);
  for (int i = 0; i < 4; i++)
    va[linoff + i] = mkpte (l2pfn[i], PTE_P | PTE_W | PTE_U);
  kva_unmap (va, PAGE_SIZE);

  /* Note: we allocate a full page for only 256 bytes. Could be optimized.

     In PAE mode CR3 is a 32-bit register: the page-directory-pointer
     table must lie below 4GB.  pfn_alloc (0) returns the highest free
     page, which is above 4GB on machines with that much memory; ask
     for the lowest one instead. */
  l3pfn = pfn_alloc (1);
  assert (l3pfn != PFN_INVALID);
  assert (ptob (l3pfn) < (1ULL << 32));

  va = kva_physmap (ptob (l3pfn), PAGE_SIZE, HAL_PTE_W | HAL_PTE_P);
  for (int i = 0; i < 4; i++)
    va[i] = mkpte (l2pfn[i], HAL_PTE_P);
  kva_unmap (va, PAGE_SIZE);

  write_cr3 (ptob (l3pfn));
}

void
pae32_init (void)
{
}
