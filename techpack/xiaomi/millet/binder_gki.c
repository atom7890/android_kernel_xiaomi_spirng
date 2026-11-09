/*
 * Copyright (c) Xiaomi Technologies Co., Ltd. 2020. All rights reserved.
 *
 * File name: oem_binder.c
 * Description: millet-binder-driver
 * Author: guchao1@xiaomi.com
 * Version: 1.0
 * Date:  2020/9/9
 */
#define pr_fmt(fmt) "millet-binder_gki: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/freezer.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/proc_fs.h>
#include "millet.h"
#include "binder_oem.h"
#include <trace/hooks/binder.h>
#include <drivers/android/binder_internal.h>

static struct hlist_head *get_binder_hhead = NULL;
static struct mutex *get_binder_lock = NULL;
static bool bgot = false;
extern unsigned int millet_debug;
extern bool millet_binder_switch;

struct oem_binder_hook oem_binder_hook_set = {
	.oem_wahead_thresh = 0,
	.oem_wahead_space = 0,
	.oem_reply_hook = NULL,
	.oem_trans_hook = NULL,
	.oem_wait4_hook = NULL,
	.oem_query_st_hook = NULL,
	.oem_buf_overflow_hook = NULL,
};

static bool binder_worklist_empty_ilocked(struct list_head *list)
{
	return list_empty(list);
}

static enum BINDER_STAT query_binder_stat(struct binder_proc *proc)
{
	struct rb_node *n;
	struct binder_thread *thread;
	struct binder_transaction *t;

	if (!oem_binder_hook_set.oem_query_st_hook)
		return BINDER_IN_IDLE;

	if (!proc->tsk)
		return BINDER_IN_IDLE;

	spin_lock(&proc->inner_lock);

	if (proc->tsk && !binder_worklist_empty_ilocked(&proc->todo)) {
		spin_unlock(&proc->inner_lock);
		return BINDER_PROC_IN_BUSY;
	}

	for (n = rb_first(&proc->threads); n != NULL; n = rb_next(n)) {
		thread = rb_entry(n, struct binder_thread, rb_node);
		if (!thread->task)
			continue;

		if (!binder_worklist_empty_ilocked(&thread->todo)) {
			spin_unlock(&proc->inner_lock);
			return BINDER_THREAD_IN_BUSY;
		}

		t = thread->transaction_stack;
		if (!t)
			continue;

		spin_lock(&t->lock);
		if (t->to_thread == thread) {
			spin_unlock(&t->lock);
			spin_unlock(&proc->inner_lock);
			return BINDER_IN_TRANSACTION;
		}
		spin_unlock(&t->lock);
	}

	spin_unlock(&proc->inner_lock);
	return BINDER_IN_IDLE;
}

static enum BINDER_STAT query_binder_stat_frozen(struct binder_proc *proc)
{
	struct list_head *w;
	struct rb_node *n;
	struct binder_thread *thread;
	struct binder_transaction *t;
	int pid;
	int need_reply;

	if (!oem_binder_hook_set.oem_query_st_hook)
		return BINDER_IN_IDLE;

	if (!proc->tsk)
		return BINDER_IN_IDLE;

	spin_lock(&proc->inner_lock);

	if (proc->tsk && !binder_worklist_empty_ilocked(&proc->todo)) {
		int caller_uid;

		caller_uid = task_uid(proc->tsk).val;
		need_reply = -1;

		for (w = proc->todo.next; w != &proc->todo; w = w->next) {
			if (!w)
				continue;

			if (container_of(w, struct binder_work, entry)->type != BINDER_WORK_TRANSACTION)
				continue;

			t = container_of(w, struct binder_transaction, work.entry);
			spin_lock(&t->lock);
			if (!t) {
				spin_unlock(&t->lock);
				continue;
			}

			if (t->to_thread) {
				spin_unlock(&t->lock);
				continue;
			}

			if (t->from && t->from->proc && t->from->proc->tsk)
				pid = t->from->proc->tsk->tgid;
			else
				pid = -1;

			need_reply = t->need_reply;
			spin_unlock(&t->lock);
			printk(KERN_ERR "millet_debug:procTodo calleruid:%d callerPid:%d targetUid:%d targetPid:%d Frozen_Trans_Proc need_reply:%d",
				t->sender_euid.val, pid, -1, -1, need_reply);

			if (need_reply == 1) {
				spin_unlock(&proc->inner_lock);
				return BINDER_PROC_IN_BUSY;
			}
		}
	}

	need_reply = -1;
	for (n = rb_first(&proc->threads); n != NULL; n = rb_next(n)) {
		thread = rb_entry(n, struct binder_thread, rb_node);
		if (!thread->task)
			continue;

		for (w = thread->todo.next; w != &thread->todo; w = w->next) {
			int target_uid, target_pid;

			if (!w)
				continue;

			if (container_of(w, struct binder_work, entry)->type != BINDER_WORK_TRANSACTION)
				continue;

			t = container_of(w, struct binder_transaction, work.entry);
			spin_lock(&t->lock);
			if (!t) {
				spin_unlock(&t->lock);
				continue;
			}

			if (t->to_thread != thread) {
				spin_unlock(&t->lock);
				continue;
			}

			if (t->from && t->from->proc && t->from->proc->tsk)
				pid = t->from->proc->tsk->tgid;
			else
				pid = -1;

			need_reply = t->need_reply;
			if (thread) {
				target_pid = thread->proc->pid;
				target_uid = task_uid(t->to_thread->task).val;
			} else {
				target_uid = -1;
				target_pid = -1;
			}
			spin_unlock(&t->lock);
			printk(KERN_ERR "millet_debug:threadTodo calleruid:%d callerPid:%d targetUid:%d targetPid:%d  Frozen_Trans_thread need_reply:%d",
				t->sender_euid.val, pid, target_uid, target_pid, need_reply);

			if (need_reply == 1) {
				spin_unlock(&proc->inner_lock);
				return BINDER_THREAD_IN_BUSY;
			}
		}

		t = thread->transaction_stack;
		if (!t)
			continue;

		spin_lock(&t->lock);
		if (t->to_thread != thread) {
			spin_unlock(&thread->transaction_stack->lock);
			continue;
		}

		pid = -1;
		if (t->from && t->from->proc && t->from->proc->tsk)
			pid = t->from->proc->tsk->tgid;

		need_reply = t->need_reply;
		if (t->to_thread) {
			int target_uid, target_pid;

			target_pid = t->to_thread->proc->pid;
			target_uid = task_uid(t->to_thread->task).val;
			printk(KERN_ERR "millet_debug:taskTodo calleruid:%d callerPid:%d targetUid:%d targetPid:%d Frozen_Trans_stack need_reply:%d",
				t->sender_euid.val, pid, target_uid, target_pid, need_reply);
			spin_unlock(&thread->transaction_stack->lock);
			if (need_reply) {
				spin_unlock(&proc->inner_lock);
				return BINDER_IN_TRANSACTION;
			}
		}
	}

	spin_unlock(&proc->inner_lock);
	return BINDER_IN_IDLE;
}

void query_binder_app_stat(int uid)
{
	struct binder_proc *proc;
	bool idle_f = true;
	enum BINDER_STAT stat;

	if (!oem_binder_hook_set.oem_query_st_hook ||
		!get_binder_lock ||!get_binder_hhead)
		return;
	mutex_lock(get_binder_lock);
	hlist_for_each_entry(proc, get_binder_hhead, proc_node) {
		enum BINDER_STAT busy;

		if (!proc->tsk)
			continue;

		if (task_uid(proc->tsk).val != uid)
			continue;

		if (millet_binder_switch)
			busy = query_binder_stat_frozen(proc);
		else
			busy = query_binder_stat(proc);

		if (busy != BINDER_IN_IDLE)
			idle_f = false;
	}

	if (idle_f)
		stat = BINDER_IN_IDLE;
	else
		stat = BINDER_IN_BUSY;

	oem_binder_hook_set.oem_query_st_hook(uid, current, 0, current->pid, stat);
	mutex_unlock(get_binder_lock);
}
EXPORT_SYMBOL_GPL(query_binder_app_stat);


struct task_struct *binder_buff_owner(size_t *free_async_space)
{
	struct binder_proc *proc = NULL;
	if (!free_async_space)
		return NULL;

	proc = container_of(free_async_space, struct binder_proc, alloc.free_async_space);
	return proc->tsk;
}

static size_t binder_alloc_buffer_size(struct binder_alloc *alloc,
				       struct binder_buffer *buffer)
{
	if (list_is_last(&buffer->entry, &alloc->buffers))
		return alloc->buffer + alloc->buffer_size - buffer->user_data + sizeof(*buffer);

	return list_next_entry(buffer, entry)->user_data - buffer->user_data + sizeof(*buffer);
}

void mi_check_buffer_size_locked(size_t size, size_t *free_async_space,
				 int is_async)
{
	struct binder_alloc *alloc;
	struct rb_node *n;
	size_t total_count = 0, total_size = 0;
	size_t async_count = 0, async_size = 0;

	if (!is_async || !free_async_space)
		return;

	if (*free_async_space > SZ_64K &&
	    size < *free_async_space - SZ_64K)
		return;

	/* Called with the allocator mutex held. Do not dereference transactions. */
	alloc = container_of(free_async_space, struct binder_alloc,
			     free_async_space);

	for (n = rb_first(&alloc->allocated_buffers); n; n = rb_next(n)) {
		struct binder_buffer *buffer;
		size_t buffer_size;

		buffer = rb_entry(n, struct binder_buffer, rb_node);
		buffer_size = binder_alloc_buffer_size(alloc, buffer);
		total_count++;
		total_size += buffer_size;

		if (buffer->async_transaction) {
			async_count++;
			async_size += buffer_size;
		}
	}

	pr_info("MIUI async allocation pid:%d requested:%zu free:%zu buffers:%zu bytes:%zu async_buffers:%zu async_bytes:%zu\\n",
		alloc->pid, size, *free_async_space, total_count, total_size,
		async_count, async_size);
}

void mi_binder_alloc_new_buf_locked(void *data, size_t size, size_t *free_async_space,
		int is_async)
{
	mi_check_buffer_size_locked(size, free_async_space, is_async);

	if (is_async && oem_binder_hook_set.oem_buf_overflow_hook
		&& ((*free_async_space < oem_binder_hook_set.oem_wahead_thresh
		* (size + sizeof(struct binder_buffer)))
		|| (*free_async_space < oem_binder_hook_set.oem_wahead_space))) {
			struct task_struct *owner;
			owner = binder_buff_owner(free_async_space);

			if (owner)
				oem_binder_hook_set.oem_buf_overflow_hook(owner, current,
						current->pid, false, 0);
	}
}

void mi_binder_replay(void *data, struct binder_proc *target_proc, struct binder_proc *proc,
	struct binder_thread *thread, struct binder_transaction_data *tr)
{
	if (oem_binder_hook_set.oem_reply_hook && target_proc->tsk)
		oem_binder_hook_set.oem_reply_hook(target_proc->tsk, proc->tsk,
				thread->pid, tr->flags & TF_ONE_WAY,
				tr->code);
}

void mi_binder_transaction(void *data, struct binder_proc *target_proc, struct binder_proc *proc,
	struct binder_thread *thread, struct binder_transaction_data *tr)
{
	if (target_proc && oem_binder_hook_set.oem_trans_hook && target_proc->tsk)
		oem_binder_hook_set.oem_trans_hook(target_proc->tsk, proc->tsk,
				thread->pid, tr->flags & TF_ONE_WAY,
				tr->code);

	if (millet_debug) {
		char desc[140];
		char buf[140];
		char *src = buf + 16;
		size_t len;
		int j;

		if (!target_proc)
			return;

		if (!(tr->flags & TF_ONE_WAY))
			return;

		if (task_uid(target_proc->tsk).val <= UID_MIN_VALUE)
			return;

		if (!frozen_task_group(target_proc->tsk))
			return;

		len = min_t(u64, tr->data_size, sizeof(buf));
		if (copy_from_user(buf, (void __user *)(unsigned long)tr->data.ptr.buffer, len))
			return;

		for (j = 0; j < sizeof(desc) - 1 &&
		     2 * j + 17 < len && src[2 * j]; j++)
			desc[j] = src[2 * j];
		desc[j] = '\0';

		printk(KERN_ERR "millet_kernel calleruid:%d callerPid:%d targetUid:%d targetPid:%d  code:%d  desc:%s",
			task_uid(proc->tsk).val, task_tgid_nr(proc->tsk),
			task_uid(target_proc->tsk).val, task_tgid_nr(target_proc->tsk),
			tr->code, desc);
	}
}

void mi_binder_wait_for_work(void *data, bool do_proc_work, struct binder_thread *thread,
	struct binder_proc *proc)
{
	struct task_struct *dst;

	if(!thread->transaction_stack)
		return;

	spin_lock(&thread->transaction_stack->lock);
	if (oem_binder_hook_set.oem_wait4_hook
			&& !thread->is_dead
			&& thread->transaction_stack
			&& thread->transaction_stack->to_proc
			&& thread->transaction_stack->to_proc->tsk){
		dst = thread->transaction_stack->to_proc->tsk;
		get_task_struct(dst);
		spin_unlock(&thread->transaction_stack->lock);
	}else{
		spin_unlock(&thread->transaction_stack->lock);
		return;
	}
	oem_binder_hook_set.oem_wait4_hook(dst,
				proc->tsk,
				thread->pid,
				thread->transaction_stack->flags & TF_ONE_WAY,
				thread->transaction_stack->code);
	put_task_struct(dst);
}

void mi_get_hhead_and_lock(void *data, struct hlist_head *hhead, struct mutex *lock)
{
	if(!bgot) {
		 if(hhead)
		 	get_binder_hhead= hhead;
		 if(lock)
		 	get_binder_lock= lock;
		 bgot= true;
	}
}

void oem_register_binder_hook(struct oem_binder_hook *set)
{
	if (!set)
		return;

	oem_binder_hook_set.oem_wahead_thresh = set->oem_wahead_thresh;
	oem_binder_hook_set.oem_wahead_space = set->oem_wahead_space;
	oem_binder_hook_set.oem_reply_hook = set->oem_reply_hook;
	oem_binder_hook_set.oem_trans_hook = set->oem_trans_hook;
	oem_binder_hook_set.oem_wait4_hook = set->oem_wait4_hook;
	oem_binder_hook_set.oem_query_st_hook = set->oem_query_st_hook;
	oem_binder_hook_set.oem_buf_overflow_hook = set->oem_buf_overflow_hook;
}
EXPORT_SYMBOL_GPL(oem_register_binder_hook);


static int __init init_binder_gki(void)
{
	pr_err("enter init_binder_gki func!\n");
	register_trace_android_vh_binder_alloc_new_buf_locked(mi_binder_alloc_new_buf_locked, NULL);
	register_trace_android_vh_binder_reply(mi_binder_replay, NULL);
	register_trace_android_vh_binder_trans(mi_binder_transaction, NULL);
	register_trace_android_vh_binder_wait_for_work(mi_binder_wait_for_work, NULL);
	register_trace_android_vh_binder_preset(mi_get_hhead_and_lock, NULL);

	return 0;
}

module_init(init_binder_gki);

MODULE_LICENSE("GPL");
