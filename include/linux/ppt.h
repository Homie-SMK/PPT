/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Sungmin Kang
 */
#ifndef _LINUX_PPT_H
#define _LINUX_PPT_H

/*
 * Page Ping-pong Throttling (PPT) for Tiered Memory Systems
 *
 * PPT prevents repeated migration of pages between memory tiers (e.g., DRAM
 * and CXL) by tracking recently promoted pages and throttling re-promotion
 * of recently demoted pages.
 *
 * Each mm_struct maintains an XArray indexed by PFN, storing:
 * - Timestamp (jiffies) of last migration
 * - pg_pingpong flag: 0=page in DRAM, 1=page in CXL
 *
 * When a page in CXL causes a NUMA fault:
 * - If pg_pingpong=1 and recently demoted: throttle (map CXL page)
 * - Otherwise: allow promotion to DRAM
 */

#include <linux/mm_types.h>
#include <linux/xarray.h>

#ifdef CONFIG_PPT

/*
 * XArray value encoding (64-bit):
 * Bit 0:      xa_mk_value tag (always 1)
 * Bits 1-22:  Jiffies timestamp (22 bits, masked)
 * Bit 23:     pg_pingpong flag (0=DRAM, 1=CXL)
 * Bits 24-63: Reserved (40 bits)
 */
#define PPT_JIFFIES_BITS       22
#define PPT_JIFFIES_MASK       ((1UL << PPT_JIFFIES_BITS) - 1)
#define PPT_PINGPONG_BIT       23
#define PPT_RESERVED_BITS      40

/* Extract fields from xarray value */
#define PPT_GET_JIFFIES(val)   (((val) >> 1) & PPT_JIFFIES_MASK)
#define PPT_GET_PINGPONG(val)  (((val) >> PPT_PINGPONG_BIT) & 1)

/* Construct xarray value */
#define PPT_MAKE_VALUE(jiffies, pingpong) \
	((((jiffies) & PPT_JIFFIES_MASK) << 1) | \
	 (((pingpong) & 1) << PPT_PINGPONG_BIT))

/*
 * Lifecycle management
 */
void ppt_mm_init(struct mm_struct *mm);
void ppt_mm_destroy(struct mm_struct *mm);
void ppt_mm_fork(struct mm_struct *oldmm, struct mm_struct *newmm);

/*
 * Promotion throttling
 *
 * Called in do_numa_page() before migrate_misplaced_folio().
 * Returns true if promotion should be throttled.
 * Sets out_flags to include TNF_THROTTLED if throttled.
 */
bool ppt_should_throttle_promotion(struct mm_struct *mm, struct page *page,
				    int *out_flags);

/*
 * Migration tracking
 *
 * Called in remove_migration_pte() after set_pte_at().
 * Updates xarray to record promotion or demotion.
 */
void ppt_track_promotion(struct mm_struct *mm, unsigned long old_pfn,
			 unsigned long new_pfn);
void ppt_track_demotion(struct mm_struct *mm, unsigned long old_pfn,
			unsigned long new_pfn);

/*
 * Statistics (exported via sysfs)
 */
struct ppt_stats {
	unsigned long promotions_allowed;
	unsigned long promotions_throttled;
	unsigned long demotions_short_lived;
	unsigned long demotions_long_lived;
	unsigned long xarray_stores_failed;
	unsigned long state_exceptions;
};

void ppt_get_stats(struct ppt_stats *stats);

#else /* !CONFIG_PPT */

static inline void ppt_mm_init(struct mm_struct *mm)
{
}

static inline void ppt_mm_destroy(struct mm_struct *mm)
{
}

static inline void ppt_mm_fork(struct mm_struct *oldmm, struct mm_struct *newmm)
{
}

static inline bool ppt_should_throttle_promotion(struct mm_struct *mm,
						  struct page *page,
						  int *out_flags)
{
	return false;
}

static inline void ppt_track_promotion(struct mm_struct *mm,
				       unsigned long old_pfn,
				       unsigned long new_pfn)
{
}

static inline void ppt_track_demotion(struct mm_struct *mm,
				      unsigned long old_pfn,
				      unsigned long new_pfn)
{
}

struct ppt_stats {
	unsigned long promotions_allowed;
	unsigned long promotions_throttled;
	unsigned long demotions_short_lived;
	unsigned long demotions_long_lived;
	unsigned long xarray_stores_failed;
	unsigned long state_exceptions;
};

static inline void ppt_get_stats(struct ppt_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
}

#endif /* CONFIG_PPT */

/*
 * Helper functions (available regardless of CONFIG_PPT)
 */

/* Get per-process PPT stats for procfs */
static inline int ppt_mm_entry_count(struct mm_struct *mm)
{
#ifdef CONFIG_PPT
	if (mm->ppt_xarray)
		return atomic_read(&mm->ppt_entry_count);
#endif
	return 0;
}

#endif /* _LINUX_PPT_H */
