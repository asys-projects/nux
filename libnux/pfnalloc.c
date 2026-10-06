/*
  NUX: A kernel Library.
  Copyright (C) 2019 Gianluca Guida, glguida@tlbflush.org

  SPDX-License-Identifier:	BSD-2-Clause
*/

#include "internal.h"
#include <string.h>
#include <nux/hal.h>
#include <nux/locks.h>
#include <nux/types.h>
#include <nux/nux.h>
#include <stree.h>
#include <assert.h>

static lock_t pglock;
static WORD_T *stree;
static unsigned order;
static unsigned long free_pages;

void
stree_pfninit (void)
{
  long first, last;

  stree = hal_physmem_stree (&order);
  assert (stree);

  first = stree_bitsearch (stree, order, 1);
  last = stree_bitsearch (stree, order, 0);
  free_pages = stree_count (stree, order);
  assert (first >= 0);
  assert (last >= 0);
  printf ("Lowest physical page free:  %08lx.\n", first);
  printf ("Highest physical page free: %08lx.\n", last);
  printf ("Memory available: %ld Kb.\n", free_pages * PAGE_SIZE / 1024);

  spinlock_init (&pglock);
}


pfn_t
stree_pfnalloc (int low)
{
  long pg;
  void *va;

  spinlock (&pglock);
  pg = stree_bitsearch (stree, order, low);
  if (pg >= 0)
    {
      assert (free_pages != 0);
      free_pages--;
      stree_clrbit (stree, order, pg);
    }
  spinunlock (&pglock);

  if (pg < 0)
    return PFN_INVALID;

  va = pfn_get (pg);
  memset (va, 0, PAGE_SIZE);
  pfn_put (pg, va);

  return (pfn_t) pg;
}

void
stree_pfnfree (pfn_t pfn)
{
  assert (pfn != PFN_INVALID);
  assert (pfn < hal_physmem_maxpfn ());
  spinlock (&pglock);
  stree_setbit (stree, order, pfn);
  free_pages++;
  spinunlock (&pglock);
}

rwlock_t _nux_pfnalloc_lock;
pfn_t (*_nux_pfnalloc) (int) = &stree_pfnalloc;
void (*_nux_pfnfree) (pfn_t) = &stree_pfnfree;

void
nux_set_allocator (pfn_t (*alloc) (int), void (*free) (pfn_t))
{
  writelock (&_nux_pfnalloc_lock);
  _nux_pfnalloc = alloc;
  _nux_pfnfree = free;
  writeunlock (&_nux_pfnalloc_lock);
}

pfn_t
pfn_alloc (int low)
{
  pfn_t pfn;

  readlock (&_nux_pfnalloc_lock);
  pfn = _nux_pfnalloc (low);
  readunlock (&_nux_pfnalloc_lock);

  return pfn;
}

void
pfn_free (pfn_t pfn)
{
  readlock (&_nux_pfnalloc_lock);
  _nux_pfnfree (pfn);
  readunlock (&_nux_pfnalloc_lock);
}

unsigned long
pfn_avail (void)
{
  return free_pages;
}
