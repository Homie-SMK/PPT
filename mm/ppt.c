// SPDX-License-Identifier: GPL-2.0
/*
 * Page Ping-pong Throttling (PPT) for Tiered Memory Systems
 *
 * Copyright (C) 2025 Sungmin Kang
 *
 * This implements a mechanism to reduce page migration overhead in tiered
 * memory systems (DRAM + CXL) by tracking and throttling repeatedly migrated
 * pages.
 */

#include <linux/ppt.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/shrinker.h>
#include <linux/memory-tiers.h>
#include <linux/node.h>
#include <linux/jiffies.h>
#include <linux/sched/numa_balancing.h>

/* Global list of mm_structs with PPT enabled */
static LIST_HEAD(ppt_mm_list_head);
static DEFINE_SPINLOCK(ppt_mm_list_lock);

/* Configuration parameters (tunable via sysfs) */
unsigned long ppt_promotion_throttle_duration = 5000;  /* ms */
unsigned long ppt_promotion_lifetime_expiration = 5000;  /* ms */
unsigned long ppt_max_entries_per_mm = 1000000;
bool ppt_enabled = false;

/* Global statistics */
static atomic64_t ppt_stats_promotions_allowed = ATOMIC64_INIT(0);
static atomic64_t ppt_stats_promotions_throttled = ATOMIC64_INIT(0);
static atomic64_t ppt_stats_demotions_short_lived = ATOMIC64_INIT(0);
static atomic64_t ppt_stats_demotions_long_lived = ATOMIC64_INIT(0);
static atomic64_t ppt_stats_xarray_stores_failed = ATOMIC64_INIT(0);
static atomic64_t ppt_stats_state_exceptions = ATOMIC64_INIT(0);

/*
 * ppt_mm_init - Initialize PPT for a new mm_struct
 * @mm: The mm_struct to initialize
 *
 * Called from mm_init() in kernel/fork.c when a new mm is created.
 * Allocates and initializes the XArray, adds mm to global tracking list.
 */
void ppt_mm_init(struct mm_struct *mm)
{
	mm->ppt_xarray = kmalloc(sizeof(struct xarray), GFP_KERNEL);
	if (!mm->ppt_xarray) {
		pr_warn("PPT: Failed to allocate xarray for mm\n");
		return;
	}

	xa_init_flags(mm->ppt_xarray, XA_FLAGS_ALLOC);
	atomic_set(&mm->ppt_entry_count, 0);
	spin_lock_init(&mm->ppt_lock);
	INIT_LIST_HEAD(&mm->ppt_mm_list);

	spin_lock(&ppt_mm_list_lock);
	list_add(&mm->ppt_mm_list, &ppt_mm_list_head);
	spin_unlock(&ppt_mm_list_lock);
}

/*
 * ppt_mm_destroy - Cleanup PPT for a dying mm_struct
 * @mm: The mm_struct being destroyed
 *
 * Called from __mmput() in kernel/fork.c when mm refcount reaches zero.
 * Removes mm from global list, destroys and frees the XArray.
 */
void ppt_mm_destroy(struct mm_struct *mm)
{
	struct xarray *xa;

	/* Use per-MM lock to protect pointer */
	spin_lock(&mm->ppt_lock);
	xa = mm->ppt_xarray;
	if (!xa) {
		spin_unlock(&mm->ppt_lock);
		return;
	}

	/* Clear pointer FIRST while holding lock to prevent new references */
	mm->ppt_xarray = NULL;
	spin_unlock(&mm->ppt_lock);

	/* Remove from global list (needs global lock) */
	spin_lock(&ppt_mm_list_lock);
	list_del(&mm->ppt_mm_list);
	spin_unlock(&ppt_mm_list_lock);

	/* Now safe to destroy - no one can get new reference to xa */
	xa_destroy(xa);
	kfree(xa);
}

/*
 * ppt_mm_fork - Setup PPT for a forked process
 * @oldmm: Parent mm_struct
 * @newmm: Child mm_struct
 *
 * Called from dup_mm() in kernel/fork.c during fork().
 * Child gets a fresh empty XArray, does not inherit parent's tracking.
 */
void ppt_mm_fork(struct mm_struct *oldmm, struct mm_struct *newmm)
{
	ppt_mm_init(newmm);
}

/*
 * ppt_should_throttle_promotion - Check if page promotion should be throttled
 * @mm: The mm_struct of the faulting process
 * @page: The page causing the NUMA fault (currently in CXL)
 * @out_flags: Output flags, TNF_THROTTLED set if throttled
 *
 * Returns: true if promotion should be throttled, false otherwise
 *
 * Called from do_numa_page() before migrate_misplaced_folio().
 * Checks if the page was recently demoted (pg_pingpong=1) and throttles
 * re-promotion if not enough time has passed.
 */
bool ppt_should_throttle_promotion(struct mm_struct *mm, struct page *page,
				    int *out_flags)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned long current_jiffies = jiffies;
	struct xarray *xa;
	void *entry;
	bool throttle = false;

	if (!ppt_enabled)
		return false;

	/* Safely get xarray pointer under per-MM lock */
	spin_lock(&mm->ppt_lock);
	xa = mm->ppt_xarray;
	if (!xa) {
		spin_unlock(&mm->ppt_lock);
		return false;
	}

	/* Lock xarray before releasing per-MM lock */
	xa_lock(xa);
	spin_unlock(&mm->ppt_lock);

	/* Now xa is locked and cannot be freed */
	entry = xa_load(xa, pfn);

	if (entry && xa_is_value(entry)) {
		unsigned long value = xa_to_value(entry);
		unsigned long stored_jiffies = PPT_GET_JIFFIES(value);
		bool pg_pingpong = PPT_GET_PINGPONG(value);
		unsigned long current_masked = current_jiffies & PPT_JIFFIES_MASK;
		unsigned long diff = (current_masked - stored_jiffies) & PPT_JIFFIES_MASK;

		if (pg_pingpong == 0) {
			/*
			 * ERROR: Page in CXL should not have pg_pingpong=0
			 * This indicates state inconsistency, remove entry
			 */
			pr_warn_once("PPT: CXL page with pg_pingpong=0 at PFN %lx\n", pfn);
			__xa_erase(xa, pfn);
			atomic_dec(&mm->ppt_entry_count);
			atomic64_inc(&ppt_stats_state_exceptions);
		} else {  /* pg_pingpong == 1 */
			if (diff < msecs_to_jiffies(ppt_promotion_throttle_duration)) {
				/*
				 * Page was recently demoted, throttle promotion
				 * to prevent ping-pong behavior
				 */
				throttle = true;
				*out_flags |= TNF_THROTTLED;
				atomic64_inc(&ppt_stats_promotions_throttled);
			} else {
				/*
				 * Enough time passed, allow promotion
				 * Remove entry, will be re-created on successful promotion
				 */
				__xa_erase(xa, pfn);
				atomic_dec(&mm->ppt_entry_count);
				atomic64_inc(&ppt_stats_promotions_allowed);
			}
		}
	} else {
		/* No entry found, first time promotion */
		atomic64_inc(&ppt_stats_promotions_allowed);
	}

	xa_unlock(xa);
	return throttle;
}

/*
 * ppt_evict_expired_entry - Evict one expired entry when at limit
 * @mm: The mm_struct to evict from
 *
 * Called when inserting a new entry would exceed max_entries_per_mm.
 * Scans for one entry past its lifetime threshold and removes it.
 */
static void ppt_evict_expired_entry(struct mm_struct *mm)
{
	unsigned long index;
	void *entry;
	unsigned long current_jiffies = jiffies & PPT_JIFFIES_MASK;
	struct xarray *xa;

	/* Safely get xarray pointer under per-MM lock */
	spin_lock(&mm->ppt_lock);
	xa = mm->ppt_xarray;
	if (!xa) {
		spin_unlock(&mm->ppt_lock);
		return;
	}
	xa_lock(xa);
	spin_unlock(&mm->ppt_lock);

	xa_for_each(xa, index, entry) {
		if (xa_is_value(entry)) {
			unsigned long value = xa_to_value(entry);
			unsigned long stored_jiffies = PPT_GET_JIFFIES(value);
			bool pg_pingpong = PPT_GET_PINGPONG(value);
			unsigned long diff = (current_jiffies - stored_jiffies) & PPT_JIFFIES_MASK;
			unsigned long threshold;

			if (pg_pingpong == 0) {
				threshold = msecs_to_jiffies(ppt_promotion_lifetime_expiration);
			} else {
				threshold = msecs_to_jiffies(ppt_promotion_throttle_duration);
			}

			if (diff >= threshold) {
				/* Entry expired, remove it */
				__xa_erase(xa, index);
				atomic_dec(&mm->ppt_entry_count);
				break;  /* Only evict one entry */
			}
		}
	}

	xa_unlock(xa);
}

/*
 * ppt_track_promotion - Record a successful page promotion
 * @mm: The mm_struct of the process
 * @old_pfn: PFN of the page in CXL (before promotion)
 * @new_pfn: PFN of the page in DRAM (after promotion)
 *
 * Called from remove_migration_pte() in mm/migrate.c after successful
 * promotion (CXL -> DRAM). Records the promotion with pg_pingpong=0.
 */
void ppt_track_promotion(struct mm_struct *mm, unsigned long old_pfn,
			 unsigned long new_pfn)
{
	unsigned long current_jiffies = jiffies & PPT_JIFFIES_MASK;
	unsigned long value = PPT_MAKE_VALUE(current_jiffies, 0);  /* pg_pingpong=0 */
	void *xa_value = xa_mk_value(value);
	struct xarray *xa;

	if (!ppt_enabled)
		return;

	/* Evict if at limit */
	if (atomic_read(&mm->ppt_entry_count) >= ppt_max_entries_per_mm) {
		ppt_evict_expired_entry(mm);
	}

	/* Safely get xarray pointer under per-MM lock */
	spin_lock(&mm->ppt_lock);
	xa = mm->ppt_xarray;
	if (!xa) {
		spin_unlock(&mm->ppt_lock);
		return;
	}
	xa_lock(xa);
	spin_unlock(&mm->ppt_lock);

	/* Remove old entry if exists */
	__xa_erase(xa, old_pfn);

	/* Insert new entry */
	if (xa_err(__xa_store(xa, new_pfn, xa_value, GFP_ATOMIC))) {
		/* Allocation failed, drop tracking for this page */
		atomic64_inc(&ppt_stats_xarray_stores_failed);
	} else {
		atomic_inc(&mm->ppt_entry_count);
	}

	xa_unlock(xa);
}

/*
 * ppt_track_demotion - Record a successful page demotion
 * @mm: The mm_struct of the process
 * @old_pfn: PFN of the page in DRAM (before demotion)
 * @new_pfn: PFN of the page in CXL (after demotion)
 *
 * Called from remove_migration_pte() in mm/migrate.c after successful
 * demotion (DRAM -> CXL).
 *
 * If page was recently promoted (short-lived in DRAM), set pg_pingpong=1
 * to enable throttling. If page was long-lived in DRAM, remove tracking.
 */
void ppt_track_demotion(struct mm_struct *mm, unsigned long old_pfn,
			unsigned long new_pfn)
{
	unsigned long current_jiffies = jiffies & PPT_JIFFIES_MASK;
	struct xarray *xa;
	void *entry;

	if (!ppt_enabled)
		return;

	/* Safely get xarray pointer under per-MM lock */
	spin_lock(&mm->ppt_lock);
	xa = mm->ppt_xarray;
	if (!xa) {
		spin_unlock(&mm->ppt_lock);
		return;
	}
	xa_lock(xa);
	spin_unlock(&mm->ppt_lock);

	entry = xa_load(xa, old_pfn);
	if (entry && xa_is_value(entry)) {
		unsigned long value = xa_to_value(entry);
		unsigned long stored_jiffies = PPT_GET_JIFFIES(value);
		unsigned long diff = (current_jiffies - stored_jiffies) & PPT_JIFFIES_MASK;

		if (diff < msecs_to_jiffies(ppt_promotion_lifetime_expiration)) {
			/*
			 * Short-lived in DRAM: page is a ping-pong candidate
			 * Update entry with pg_pingpong=1 to throttle future promotions
			 */
			unsigned long new_value = PPT_MAKE_VALUE(current_jiffies, 1);
			void *xa_value = xa_mk_value(new_value);

			__xa_erase(xa, old_pfn);
			if (xa_err(__xa_store(xa, new_pfn, xa_value, GFP_ATOMIC))) {
				atomic64_inc(&ppt_stats_xarray_stores_failed);
				atomic_dec(&mm->ppt_entry_count);
			} else {
				atomic64_inc(&ppt_stats_demotions_short_lived);
			}
		} else {
			/*
			 * Long-lived in DRAM: page is not a ping-pong page
			 * Remove tracking, no need to throttle future promotions
			 */
			__xa_erase(xa, old_pfn);
			atomic_dec(&mm->ppt_entry_count);
			atomic64_inc(&ppt_stats_demotions_long_lived);
		}
	}

	xa_unlock(xa);
}

/*
 * ppt_shrink_mm_xarray - Shrink one mm's xarray
 * @mm: The mm_struct to shrink
 * @nr_to_scan: Maximum number of entries to scan/remove
 *
 * Returns: Number of entries freed
 *
 * Scans xarray for expired entries and removes them.
 */
static unsigned long ppt_shrink_mm_xarray(struct mm_struct *mm,
					  unsigned long nr_to_scan)
{
	unsigned long index;
	void *entry;
	unsigned long current_jiffies = jiffies & PPT_JIFFIES_MASK;
	unsigned long freed = 0;
	struct xarray *xa;

	/* Safely get xarray pointer under per-MM lock */
	spin_lock(&mm->ppt_lock);
	xa = mm->ppt_xarray;
	if (!xa) {
		spin_unlock(&mm->ppt_lock);
		return 0;
	}
	xa_lock(xa);
	spin_unlock(&mm->ppt_lock);

	xa_for_each(xa, index, entry) {
		if (freed >= nr_to_scan)
			break;

		if (xa_is_value(entry)) {
			unsigned long value = xa_to_value(entry);
			unsigned long stored_jiffies = PPT_GET_JIFFIES(value);
			bool pg_pingpong = PPT_GET_PINGPONG(value);
			unsigned long diff = (current_jiffies - stored_jiffies) & PPT_JIFFIES_MASK;
			unsigned long threshold;

			if (pg_pingpong == 0) {
				threshold = msecs_to_jiffies(ppt_promotion_lifetime_expiration);
			} else {
				threshold = msecs_to_jiffies(ppt_promotion_throttle_duration);
			}

			if (diff >= threshold) {
				/* Entry expired, remove it */
				__xa_erase(xa, index);
				atomic_dec(&mm->ppt_entry_count);
				freed++;
			}
		}
	}

	xa_unlock(xa);

	return freed;
}

/*
 * ppt_shrinker_count - Count reclaimable PPT entries
 * @shrink: Shrinker instance
 * @sc: Shrink control
 *
 * Returns: Total number of tracked entries across all mm_structs
 */
static unsigned long ppt_shrinker_count(struct shrinker *shrink,
					struct shrink_control *sc)
{
	struct mm_struct *mm;
	unsigned long total = 0;

	spin_lock(&ppt_mm_list_lock);
	list_for_each_entry(mm, &ppt_mm_list_head, ppt_mm_list) {
		total += atomic_read(&mm->ppt_entry_count);
	}
	spin_unlock(&ppt_mm_list_lock);

	return total;
}

/*
 * ppt_shrinker_scan - Scan and free expired PPT entries
 * @shrink: Shrinker instance
 * @sc: Shrink control
 *
 * Returns: Number of entries freed
 *
 * Iterates through all mm_structs and removes expired entries.
 */
static unsigned long ppt_shrinker_scan(struct shrinker *shrink,
				       struct shrink_control *sc)
{
	struct mm_struct *mm;
	unsigned long freed = 0;
	unsigned long to_scan = sc->nr_to_scan;

	spin_lock(&ppt_mm_list_lock);
	list_for_each_entry(mm, &ppt_mm_list_head, ppt_mm_list) {
		if (freed >= to_scan)
			break;

		freed += ppt_shrink_mm_xarray(mm, to_scan - freed);
	}
	spin_unlock(&ppt_mm_list_lock);

	return freed;
}

/* Shrinker pointer */
static struct shrinker *ppt_shrinker;

/*
 * ppt_shrinker_init - Register PPT shrinker
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Called at late_initcall to register the shrinker with the kernel.
 */
static int __init ppt_shrinker_init(void)
{
	ppt_shrinker = shrinker_alloc(0, "ppt");
	if (!ppt_shrinker) {
		pr_err("PPT: Failed to allocate shrinker\n");
		return -ENOMEM;
	}

	ppt_shrinker->count_objects = ppt_shrinker_count;
	ppt_shrinker->scan_objects = ppt_shrinker_scan;
	ppt_shrinker->seeks = DEFAULT_SEEKS;

	shrinker_register(ppt_shrinker);

	pr_info("PPT: Shrinker registered successfully\n");
	return 0;
}
late_initcall(ppt_shrinker_init);

/*
 * ppt_get_stats - Get global PPT statistics
 * @stats: Output structure to fill
 *
 * Copies current statistics to the provided structure.
 * Used by sysfs interface to export stats to userspace.
 */
void ppt_get_stats(struct ppt_stats *stats)
{
	stats->promotions_allowed = atomic64_read(&ppt_stats_promotions_allowed);
	stats->promotions_throttled = atomic64_read(&ppt_stats_promotions_throttled);
	stats->demotions_short_lived = atomic64_read(&ppt_stats_demotions_short_lived);
	stats->demotions_long_lived = atomic64_read(&ppt_stats_demotions_long_lived);
	stats->xarray_stores_failed = atomic64_read(&ppt_stats_xarray_stores_failed);
	stats->state_exceptions = atomic64_read(&ppt_stats_state_exceptions);
}
EXPORT_SYMBOL_GPL(ppt_get_stats);
