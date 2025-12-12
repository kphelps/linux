// SPDX-License-Identifier: GPL-2.0
/*
 * Gemvisor guest PFN allocation bitmap implementation.
 *
 * The bitmap is a stable, guest-maintained view of which PFNs are currently
 * allocated (in-use). It is exported to the hypervisor so snapshots can omit
 * free-page contents deterministically. To make that safe, we scrub pages to
 * zero on free.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/mmzone.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/list.h>

#include <asm/kvm_para.h>
#include <asm/gemvisor_alloc_bitmap.h>

static unsigned long *gem_alloc_bitmap;
static unsigned long gem_alloc_bitmap_nbits;
static unsigned long gem_alloc_bitmap_len_bytes;
static bool gem_alloc_bitmap_ready;

static inline void gem_alloc_bitmap_set_range(unsigned long start_pfn,
					      unsigned int order, bool allocated)
{
	unsigned long count = 1UL << order;
	unsigned long pfn;

	for (pfn = start_pfn; pfn < start_pfn + count; pfn++) {
		if (pfn >= gem_alloc_bitmap_nbits)
			break;
		if (allocated)
			set_bit(pfn, gem_alloc_bitmap);
		else
			clear_bit(pfn, gem_alloc_bitmap);
	}
}

void gemvisor_alloc_bitmap_mark_alloc(struct page *page, unsigned int order)
{
	unsigned long start_pfn;

	if (!page || !gem_alloc_bitmap_ready)
		return;

	start_pfn = page_to_pfn(page);
	gem_alloc_bitmap_set_range(start_pfn, order, true);
}

void gemvisor_alloc_bitmap_mark_free_and_scrub(struct page *page, unsigned int order)
{
	unsigned long start_pfn;
	unsigned long count = 1UL << order;
	unsigned long pfn;

	if (!page)
		return;

	start_pfn = page_to_pfn(page);
	for (pfn = start_pfn; pfn < start_pfn + count; pfn++) {
		if (pfn >= gem_alloc_bitmap_nbits)
			break;
		if (gem_alloc_bitmap_ready)
			clear_bit(pfn, gem_alloc_bitmap);
		clear_highpage(pfn_to_page(pfn));
	}
}

static int __init gemvisor_alloc_bitmap_init(void)
{
	unsigned long nr_pages = totalram_pages();
	unsigned long nlongs = BITS_TO_LONGS(nr_pages);
	unsigned long bytes = nlongs * sizeof(unsigned long);
	unsigned int order;
	struct page *bitmap_pages;
	unsigned long phys;
	struct zone *zone;
	unsigned int o, mt;

	if (!nr_pages)
		return 0;

	order = get_order(bytes);
	bitmap_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!bitmap_pages) {
		pr_warn("gemvisor-alloc-bitmap: alloc_pages failed\n");
		return 0;
	}

	gem_alloc_bitmap = page_address(bitmap_pages);
	gem_alloc_bitmap_nbits = nr_pages;
	gem_alloc_bitmap_len_bytes = bytes;

	/* Start with all PFNs marked allocated, then clear buddy free lists. */
	bitmap_fill(gem_alloc_bitmap, gem_alloc_bitmap_nbits);

	for_each_populated_zone(zone) {
		spin_lock(&zone->lock);
		for (o = 0; o < MAX_ORDER; o++) {
			struct free_area *area = &zone->free_area[o];

			for (mt = 0; mt < MIGRATE_TYPES; mt++) {
				struct page *p;

				list_for_each_entry(p, &area->free_list[mt], lru) {
					unsigned long pfn = page_to_pfn(p);
					gem_alloc_bitmap_set_range(pfn, o, false);
				}
			}
		}
		spin_unlock(&zone->lock);
	}

	phys = (unsigned long)page_to_pfn(bitmap_pages) << PAGE_SHIFT;
	kvm_hypercall2(GEMVISOR_HC_ALLOC_BITMAP_REGISTER,
		       (unsigned long)phys,
		       (unsigned long)gem_alloc_bitmap_len_bytes);

	gem_alloc_bitmap_ready = true;
	pr_info("gemvisor-alloc-bitmap: registered %lu PFNs (%lu bytes) at GPA %#lx\n",
		gem_alloc_bitmap_nbits, gem_alloc_bitmap_len_bytes, phys);

	return 0;
}
core_initcall(gemvisor_alloc_bitmap_init);
