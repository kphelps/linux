/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Gemvisor guest PFN allocation bitmap.
 *
 * The guest maintains an in-use bitmap over PFNs (1=allocated, 0=free).
 * Free pages are scrubbed to zero on free so the hypervisor can safely
 * prune free-page contents from snapshots while preserving determinism.
 */
#ifndef _ASM_X86_GEMVISOR_ALLOC_BITMAP_H
#define _ASM_X86_GEMVISOR_ALLOC_BITMAP_H

#include <linux/types.h>
#include <linux/mm_types.h>

/* Hypercall: register allocation bitmap with the hypervisor. */
#define GEMVISOR_HC_ALLOC_BITMAP_REGISTER 0x47454D04 /* "GEM\x04" */

void gemvisor_alloc_bitmap_mark_alloc(struct page *page, unsigned int order);
void gemvisor_alloc_bitmap_mark_free_and_scrub(struct page *page, unsigned int order);

#endif /* _ASM_X86_GEMVISOR_ALLOC_BITMAP_H */

