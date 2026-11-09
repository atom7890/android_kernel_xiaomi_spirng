// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Xiaomi Ltd.
 */
#define pr_fmt(fmt) "shrink_async: " fmt

#include <linux/swap.h>
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/topology.h>
#include <linux/proc_fs.h>
#include <linux/gfp.h>
#include <linux/types.h>
#include <linux/cpufreq.h>
#include <linux/freezer.h>
#include <linux/wait.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/stat.h>
#include <linux/module.h>
#include <trace/hooks/vmscan.h>


#define SHRINK_SLABD_NAME "kshrink_slabd"
extern unsigned long shrink_slab(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg,
				 int priority);

static struct task_struct *shrink_slabd_tsk = NULL;
static bool async_shrink_slabd_setup = false;
wait_queue_head_t shrink_slabd_wait;
static DEFINE_SPINLOCK(request_lock);

struct async_slabd_parameter {
	struct mem_cgroup *shrink_slabd_memcg;
	gfp_t shrink_slabd_gfp_mask;
	atomic_t shrink_slabd_runnable;
	int shrink_slabd_nid;
	int priority;
} asp;


bool wakeup_shrink_slabd(gfp_t gfp_mask, int nid,
			struct mem_cgroup *memcg, int priority)
{
	unsigned long flags;
	bool accepted = false;

	spin_lock_irqsave(&request_lock, flags);
	if (!async_shrink_slabd_setup ||
	    atomic_read(&asp.shrink_slabd_runnable))
		goto unlock;
	if (!mem_cgroup_tryget(memcg))
		goto unlock;

	asp.shrink_slabd_gfp_mask = gfp_mask;
	asp.shrink_slabd_nid = nid;
	asp.shrink_slabd_memcg = memcg;
	asp.priority = priority;
	atomic_set(&asp.shrink_slabd_runnable, 1);
	accepted = true;
unlock:
	spin_unlock_irqrestore(&request_lock, flags);
	if (accepted)
		wake_up_interruptible(&shrink_slabd_wait);
	return accepted;
}


void set_async_slabd_cpus(void)
{
	cpumask_t fastest, allowed;
	unsigned int cpu, max_freq = 0;
	bool found = false;
	static bool configured;

	if (configured)
		return;

	cpumask_clear(&fastest);
	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

		if (!policy)
			continue;
		if (!found || policy->cpuinfo.max_freq >= max_freq) {
			max_freq = policy->cpuinfo.max_freq;
			cpumask_copy(&fastest, policy->related_cpus);
			found = true;
		}
		cpufreq_cpu_put(policy);
	}
	if (!found)
		return;

	cpumask_and(&allowed, cpumask_of_node(0), cpu_online_mask);
	cpumask_andnot(&allowed, &allowed, &fastest);
	if (!cpumask_empty(&allowed) &&
	    !set_cpus_allowed_ptr(current, &allowed))
		configured = true;
}


static int kshrink_slabd_func(void *p)
{
	unsigned long flags;
	struct mem_cgroup *memcg;

	current->flags |= PF_MEMALLOC | PF_KSWAPD;
	set_freezable();

	for (;;) {
		gfp_t gfp_mask;
		int nid, priority;
		int ret;

		ret = wait_event_freezable(shrink_slabd_wait,
			kthread_should_stop() ||
			atomic_read(&asp.shrink_slabd_runnable));
		if (kthread_should_stop())
			break;
		if (ret)
			continue;

		spin_lock_irqsave(&request_lock, flags);
		memcg = asp.shrink_slabd_memcg;
		gfp_mask = asp.shrink_slabd_gfp_mask;
		nid = asp.shrink_slabd_nid;
		priority = asp.priority;
		spin_unlock_irqrestore(&request_lock, flags);

		set_async_slabd_cpus();
		shrink_slab(gfp_mask, nid, memcg, priority);
		mem_cgroup_put(memcg);

		spin_lock_irqsave(&request_lock, flags);
		asp.shrink_slabd_memcg = NULL;
		atomic_set(&asp.shrink_slabd_runnable, 0);
		spin_unlock_irqrestore(&request_lock, flags);
	}

	/* Release a queued request if shutdown happened before processing it. */
	spin_lock_irqsave(&request_lock, flags);
	memcg = asp.shrink_slabd_memcg;
	asp.shrink_slabd_memcg = NULL;
	atomic_set(&asp.shrink_slabd_runnable, 0);
	spin_unlock_irqrestore(&request_lock, flags);
	mem_cgroup_put(memcg);

	current->flags &= ~(PF_MEMALLOC | PF_KSWAPD);
	return 0;
}


static void should_shrink_async(void *data, gfp_t gfp_mask, int nid,
			struct mem_cgroup *memcg, int priority, bool *bypass)
{
	static unsigned long prev_jiffies;
	unsigned long flags, now, elapsed;
	bool ready;

	if (current == READ_ONCE(shrink_slabd_tsk))
		return;

	spin_lock_irqsave(&request_lock, flags);
	ready = async_shrink_slabd_setup;
	now = jiffies;
	elapsed = now - prev_jiffies;
	if (ready)
		prev_jiffies = now;
	spin_unlock_irqrestore(&request_lock, flags);

	if (!ready || elapsed < HZ || *bypass)
		return;

	if (wakeup_shrink_slabd(gfp_mask, nid, memcg, priority))
		*bypass = true;
}

static int register_shrink_async_vendor_hooks(void)
{
	int ret = 0;

	ret = register_trace_android_vh_shrink_slab_bypass(should_shrink_async, NULL);
	if (ret != 0) {
		pr_err("register_trace_android_vh_shrink_slab_bypass failed! ret=%d\n", ret);
		goto out;
	}
out:
	return ret;
}

static void unregister_shrink_async_vendor_hooks(void)
{
	unregister_trace_android_vh_shrink_slab_bypass(should_shrink_async, NULL);

	return;
}



static int __init shrink_async_init(void)
{
	unsigned long flags;
	int ret;

	init_waitqueue_head(&shrink_slabd_wait);
	atomic_set(&asp.shrink_slabd_runnable, 0);

	shrink_slabd_tsk = kthread_run(kshrink_slabd_func, NULL,
				     SHRINK_SLABD_NAME);
	if (IS_ERR(shrink_slabd_tsk)) {
		ret = PTR_ERR(shrink_slabd_tsk);
		shrink_slabd_tsk = NULL;
		return ret;
	}

	spin_lock_irqsave(&request_lock, flags);
	async_shrink_slabd_setup = true;
	spin_unlock_irqrestore(&request_lock, flags);

	ret = register_shrink_async_vendor_hooks();
	if (ret) {
		spin_lock_irqsave(&request_lock, flags);
		async_shrink_slabd_setup = false;
		spin_unlock_irqrestore(&request_lock, flags);
		kthread_stop(shrink_slabd_tsk);
		shrink_slabd_tsk = NULL;
	}
	return ret;
}

static void __exit shrink_async_exit(void)
{
	unsigned long flags;

	spin_lock_irqsave(&request_lock, flags);
	async_shrink_slabd_setup = false;
	spin_unlock_irqrestore(&request_lock, flags);

	unregister_shrink_async_vendor_hooks();
	tracepoint_synchronize_unregister();
	kthread_stop(shrink_slabd_tsk);
	shrink_slabd_tsk = NULL;
}

module_init(shrink_async_init);
module_exit(shrink_async_exit);

MODULE_LICENSE("GPL v2");
