// SPDX-License-Identifier: GPL-2.0
/*
 * Sysfs interface for Page Ping-pong Throttling (PPT)
 *
 * Copyright (C) 2025 Sungmin Kang
 */

#include <linux/ppt.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mm.h>

/* External mm_kobj from mm/mm_init.c */
extern struct kobject *mm_kobj;

/* External configuration parameters from mm/ppt.c */
extern unsigned long ppt_promotion_throttle_duration;
extern unsigned long ppt_promotion_lifetime_expiration;
extern unsigned long ppt_max_entries_per_mm;
extern bool ppt_enabled;

/*
 * enabled - Enable/disable PPT globally
 */
static ssize_t enabled_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", ppt_enabled);
}

static ssize_t enabled_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	int val;
	int ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	if (val < 0 || val > 1)
		return -EINVAL;

	ppt_enabled = !!val;
	return count;
}

static struct kobj_attribute enabled_attr =
	__ATTR(enabled, 0644, enabled_show, enabled_store);

/*
 * promotion_throttle_duration - Time window for throttling (milliseconds)
 */
static ssize_t promotion_throttle_duration_show(struct kobject *kobj,
						struct kobj_attribute *attr,
						char *buf)
{
	return sprintf(buf, "%lu\n", ppt_promotion_throttle_duration);
}

static ssize_t promotion_throttle_duration_store(struct kobject *kobj,
						 struct kobj_attribute *attr,
						 const char *buf, size_t count)
{
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	/* Reasonable bounds: 1ms to 10 minutes */
	if (val < 1 || val > 600000)
		return -EINVAL;

	ppt_promotion_throttle_duration = val;
	return count;
}

static struct kobj_attribute promotion_throttle_duration_attr =
	__ATTR(promotion_throttle_duration, 0644,
	       promotion_throttle_duration_show,
	       promotion_throttle_duration_store);

/*
 * promotion_lifetime_expiration - Lifetime threshold (milliseconds)
 */
static ssize_t promotion_lifetime_expiration_show(struct kobject *kobj,
						  struct kobj_attribute *attr,
						  char *buf)
{
	return sprintf(buf, "%lu\n", ppt_promotion_lifetime_expiration);
}

static ssize_t promotion_lifetime_expiration_store(struct kobject *kobj,
						   struct kobj_attribute *attr,
						   const char *buf, size_t count)
{
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	/* Reasonable bounds: 1ms to 10 minutes */
	if (val < 1 || val > 600000)
		return -EINVAL;

	ppt_promotion_lifetime_expiration = val;
	return count;
}

static struct kobj_attribute promotion_lifetime_expiration_attr =
	__ATTR(promotion_lifetime_expiration, 0644,
	       promotion_lifetime_expiration_show,
	       promotion_lifetime_expiration_store);

/*
 * max_entries_per_mm - Maximum xarray entries per process
 */
static ssize_t max_entries_per_mm_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", ppt_max_entries_per_mm);
}

static ssize_t max_entries_per_mm_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	/* Reasonable bounds: 1000 to 10 million */
	if (val < 1000 || val > 10000000)
		return -EINVAL;

	ppt_max_entries_per_mm = val;
	return count;
}

static struct kobj_attribute max_entries_per_mm_attr =
	__ATTR(max_entries_per_mm, 0644,
	       max_entries_per_mm_show, max_entries_per_mm_store);

/*
 * Statistics (read-only)
 */
static ssize_t promotions_allowed_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	struct ppt_stats stats;
	ppt_get_stats(&stats);
	return sprintf(buf, "%lu\n", stats.promotions_allowed);
}

static struct kobj_attribute promotions_allowed_attr =
	__ATTR_RO(promotions_allowed);

static ssize_t promotions_throttled_show(struct kobject *kobj,
					 struct kobj_attribute *attr, char *buf)
{
	struct ppt_stats stats;
	ppt_get_stats(&stats);
	return sprintf(buf, "%lu\n", stats.promotions_throttled);
}

static struct kobj_attribute promotions_throttled_attr =
	__ATTR_RO(promotions_throttled);

static ssize_t demotions_short_lived_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	struct ppt_stats stats;
	ppt_get_stats(&stats);
	return sprintf(buf, "%lu\n", stats.demotions_short_lived);
}

static struct kobj_attribute demotions_short_lived_attr =
	__ATTR_RO(demotions_short_lived);

static ssize_t demotions_long_lived_show(struct kobject *kobj,
					 struct kobj_attribute *attr, char *buf)
{
	struct ppt_stats stats;
	ppt_get_stats(&stats);
	return sprintf(buf, "%lu\n", stats.demotions_long_lived);
}

static struct kobj_attribute demotions_long_lived_attr =
	__ATTR_RO(demotions_long_lived);

static ssize_t xarray_stores_failed_show(struct kobject *kobj,
					 struct kobj_attribute *attr, char *buf)
{
	struct ppt_stats stats;
	ppt_get_stats(&stats);
	return sprintf(buf, "%lu\n", stats.xarray_stores_failed);
}

static struct kobj_attribute xarray_stores_failed_attr =
	__ATTR_RO(xarray_stores_failed);

static ssize_t state_exceptions_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	struct ppt_stats stats;
	ppt_get_stats(&stats);
	return sprintf(buf, "%lu\n", stats.state_exceptions);
}

static struct kobj_attribute state_exceptions_attr =
	__ATTR_RO(state_exceptions);

/* Attribute groups */
static struct attribute *ppt_attrs[] = {
	&enabled_attr.attr,
	&promotion_throttle_duration_attr.attr,
	&promotion_lifetime_expiration_attr.attr,
	&max_entries_per_mm_attr.attr,
	&promotions_allowed_attr.attr,
	&promotions_throttled_attr.attr,
	&demotions_short_lived_attr.attr,
	&demotions_long_lived_attr.attr,
	&xarray_stores_failed_attr.attr,
	&state_exceptions_attr.attr,
	NULL,
};

static const struct attribute_group ppt_attr_group = {
	.attrs = ppt_attrs,
	.name = "ppt",
};

/*
 * ppt_sysfs_init - Initialize PPT sysfs interface
 *
 * Creates /sys/kernel/mm/ppt/ directory with all attributes.
 */
static int __init ppt_sysfs_init(void)
{
	int ret;

	if (!mm_kobj) {
		pr_err("PPT: mm_kobj not available\n");
		return -ENOENT;
	}

	/* Create /sys/kernel/mm/ppt/ */
	ret = sysfs_create_group(mm_kobj, &ppt_attr_group);
	if (ret) {
		pr_err("PPT: Failed to create sysfs group: %d\n", ret);
		return ret;
	}

	pr_info("PPT: Sysfs interface created at /sys/kernel/mm/ppt/\n");
	return 0;
}
late_initcall(ppt_sysfs_init);
