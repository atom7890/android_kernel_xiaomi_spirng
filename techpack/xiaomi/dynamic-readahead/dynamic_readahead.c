// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spring reconstruction from the working module's observed behavior.
 */
#define pr_fmt(fmt) "dynamic_readahead: " fmt

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/vmstat.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/cgroup.h>
#include <linux/kernfs.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/rcupdate.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <trace/hooks/vendor_hooks.h>

/*
 * Consumer declarations only. The tracepoints are supplied by common GKI.
 * Avoid importing a different kernel's complete trace/hooks/mm.h.
 */
DECLARE_HOOK(android_vh_ra_tuning_max_page,
	TP_PROTO(struct readahead_control *ractl, unsigned long *max_page),
	TP_ARGS(ractl, max_page));

DECLARE_HOOK(android_vh_tune_mmap_readaround,
	TP_PROTO(unsigned int ra_pages, pgoff_t pgoff,
		 pgoff_t *start, unsigned int *size, unsigned int *async_size),
	TP_ARGS(ra_pages, pgoff, start, size, async_size));

DECLARE_HOOK(android_vh_should_fault_around,
	TP_PROTO(struct vm_fault *vmf, bool *should_around),
	TP_ARGS(vmf, should_around));

static bool enable = true;
module_param(enable, bool, 0600);
MODULE_PARM_DESC(enable,
	"dynamic_readahead.enable=1 to indicate supporting dynamic readahead or not ");

static unsigned long long high_wm;

static bool reduce_readahead(void)
{
	bool background = false;

	if (!READ_ONCE(enable))
		return false;

#ifdef CONFIG_CPUSETS
	rcu_read_lock();
	{
		struct cgroup_subsys_state *css =
			task_css(current, cpuset_cgrp_id);

		if (css && css->cgroup && css->cgroup->kn)
			background = !strncmp(css->cgroup->kn->name,
					      "background", 11);
	}
	rcu_read_unlock();
#endif

	return background && !rt_task(current) &&
		global_zone_page_state(NR_FREE_PAGES) < high_wm;
}

static void adjust_readahead(void *unused, struct readahead_control *ractl,
			     unsigned long *max_page)
{
	if (reduce_readahead())
		*max_page = min_t(unsigned long, *max_page,
				 ractl->ra->ra_pages / 2);
}

static void adjust_readaround(void *unused, unsigned int ra_pages,
			      pgoff_t pgoff, pgoff_t *start,
			      unsigned int *size, unsigned int *async_size)
{
	if (!reduce_readahead())
		return;

	*start = pgoff > ra_pages / 4 ? pgoff - ra_pages / 4 : 0;
	*size = ra_pages / 2;
	*async_size = ra_pages / 8;
}

static void should_fault_around(void *unused, struct vm_fault *vmf,
				bool *should_around)
{
	/* The reference callback is unconditional, independent of enable. */
	*should_around = false;
}

static int __init dynamic_readahead_init(void)
{
	int ret, i;
	pg_data_t *pgdat = NODE_DATA(0);

	/* Spring's reference module iterates the node-zero zones. */
	for (i = 0; i < MAX_NR_ZONES; i++)
		high_wm += high_wmark_pages((&pgdat->node_zones[i]));

	ret = register_trace_android_vh_ra_tuning_max_page(
		adjust_readahead, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_vh_tune_mmap_readaround(
		adjust_readaround, NULL);
	if (ret)
		goto unregister_ra;

	ret = register_trace_android_vh_should_fault_around(
		should_fault_around, NULL);
	if (ret)
		goto unregister_readaround;

	pr_info("dynamic_readahead_init high_wm: %llu\n", high_wm);
	return 0;

unregister_readaround:
	unregister_trace_android_vh_tune_mmap_readaround(
		adjust_readaround, NULL);
unregister_ra:
	unregister_trace_android_vh_ra_tuning_max_page(
		adjust_readahead, NULL);
	tracepoint_synchronize_unregister();
	return ret;
}

static void __exit dynamic_readahead_exit(void)
{
	unregister_trace_android_vh_should_fault_around(
		should_fault_around, NULL);
	unregister_trace_android_vh_tune_mmap_readaround(
		adjust_readaround, NULL);
	unregister_trace_android_vh_ra_tuning_max_page(
		adjust_readahead, NULL);
	tracepoint_synchronize_unregister();
}

module_init(dynamic_readahead_init);
module_exit(dynamic_readahead_exit);
MODULE_LICENSE("GPL");
