// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spring quick-reclaim policy reconstructed from the retained module.
 * This is not the original Xiaomi source.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/mm.h>
#include <linux/vmstat.h>

/* Defined privately in mm/vmscan.c; preserve its exact enum definition. */
enum scan_balance {
	SCAN_EQUAL,
	SCAN_FRACT,
	SCAN_ANON,
	SCAN_FILE,
};

#include <trace/hooks/vmscan.h>

#define QUICK_RECLAIM_FILE_PAGES 133120UL

static bool need_quick_reclaim(void)
{
	struct task_struct *leader;
	char comm[TASK_COMM_LEN];

	get_task_comm(comm, current);
	if (!strcmp(comm, "swapon01") ||
	    !strncmp(comm, "MiuiIntegratedM", 15))
		return false;

	if (global_node_page_state(NR_INACTIVE_FILE) <=
	    QUICK_RECLAIM_FILE_PAGES)
		return false;

	if (READ_ONCE(current->flags) & PF_KSWAPD)
		return (global_zone_page_state(NR_FREE_PAGES) >> 9) <= 8;

	/* The current task keeps its thread-group leader alive. */
	leader = current->group_leader;
	if (READ_ONCE(leader->signal->oom_score_adj) <= 0)
		return true;

	get_task_comm(comm, leader);
	return !strcmp(comm, "com.miui.home") ||
	       !strcmp(comm, "com.android.systemui");
}

static void tune_scan_type(void *unused, enum scan_balance *scan_type)
{
	if (scan_type && *scan_type != SCAN_FILE && need_quick_reclaim())
		*scan_type = SCAN_FILE;
}

static void should_shrink_bypass(void *unused, gfp_t gfp_mask, int nid,
				struct mem_cgroup *memcg, int priority,
				bool *bypass)
{
	/* Preserve decisions made by other callbacks. */
	if (need_quick_reclaim())
		*bypass = true;
}

static int __init mi_memqr_init(void)
{
	int ret;

	ret = register_trace_android_vh_tune_scan_type(tune_scan_type, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_vh_shrink_slab_bypass(
		should_shrink_bypass, NULL);
	if (ret) {
		unregister_trace_android_vh_tune_scan_type(tune_scan_type, NULL);
		tracepoint_synchronize_unregister();
		return ret;
	}

	pr_info("mem quickreclaim init succeed!\n");
	return 0;
}

static void __exit mi_memqr_exit(void)
{
	unregister_trace_android_vh_shrink_slab_bypass(
		should_shrink_bypass, NULL);
	unregister_trace_android_vh_tune_scan_type(tune_scan_type, NULL);
	tracepoint_synchronize_unregister();
}

module_init(mi_memqr_init);
module_exit(mi_memqr_exit);

MODULE_DESCRIPTION("Spring memory quick reclaim reconstruction");
MODULE_LICENSE("GPL");
