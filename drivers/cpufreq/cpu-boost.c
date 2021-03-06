/*
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "cpu-boost: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/time.h>

struct cpu_sync {
	struct task_struct *thread;
	wait_queue_head_t sync_wq;
	struct delayed_work boost_rem;
	int cpu;
	spinlock_t lock;
	bool pending;
	int src_cpu;
	unsigned int boost_min;
	unsigned int task_load;
};

static DEFINE_PER_CPU(struct cpu_sync, sync_info);
static struct workqueue_struct *cpu_boost_wq;

static unsigned int boost_ms;
module_param(boost_ms, uint, 0644);

static unsigned int sync_threshold;
module_param(sync_threshold, uint, 0644);

unsigned int input_boost_freq = 1497600;
module_param(input_boost_freq, uint, 0644);

unsigned int input_boost_ms = 1500;
module_param(input_boost_ms, uint, 0644);

static unsigned int migration_load_threshold = 15;
module_param(migration_load_threshold, uint, 0644);

static bool load_based_syncs;
module_param(load_based_syncs, bool, 0644);

/* Get input_boost duration in uS */
int get_input_boost_duration(void)
{
	return input_boost_ms * 1000;
}

/*
 * The CPUFREQ_ADJUST notifier is used to override the current policy min to
 * make sure policy min >= boost_min. The cpufreq framework then does the job
 * of enforcing the new policy.
 *
 * The sync kthread needs to run on the CPU in question to avoid deadlocks in
 * the wake up code. Achieve this by binding the thread to the respective
 * CPU. But a CPU going offline unbinds threads from that CPU. So, set it up
 * again each time the CPU comes back up. We can use CPUFREQ_START to figure
 * out a CPU is coming online instead of registering for hotplug notifiers.
 */
static int boost_adjust_notify(struct notifier_block *nb, unsigned long val,
				void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu = policy->cpu;
	struct cpu_sync *s = &per_cpu(sync_info, cpu);
	unsigned int b_min = s->boost_min;
	unsigned int min;

	switch (val) {
	case CPUFREQ_ADJUST:
		if (!b_min)
			break;

		min = b_min;

		pr_debug("CPU%u policy min before boost: %u kHz\n",
			 cpu, policy->min);
		pr_debug("CPU%u boost min: %u kHz\n", cpu, min);

		cpufreq_verify_within_limits(policy, min, UINT_MAX);

		pr_debug("CPU%u policy min after boost: %u kHz\n",
			 cpu, policy->min);
		break;

	case CPUFREQ_START:
		set_cpus_allowed(s->thread, *cpumask_of(cpu));
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block boost_adjust_nb = {
	.notifier_call = boost_adjust_notify,
};

static void do_boost_rem(struct work_struct *work)
{
	struct cpu_sync *s = container_of(work, struct cpu_sync,
						boost_rem.work);

	pr_debug("Removing boost for CPU%d\n", s->cpu);
	s->boost_min = 0;
	/* Force policy re-evaluation to trigger adjust notifier. */
	cpufreq_update_policy(s->cpu);
}

static int boost_mig_sync_thread(void *data)
{
	int dest_cpu = (int) data;
	int src_cpu, ret;
	struct cpu_sync *s = &per_cpu(sync_info, dest_cpu);
	struct cpufreq_policy dest_policy;
	struct cpufreq_policy src_policy;
	unsigned long flags;
	unsigned int req_freq;

	while (1) {
		wait_event(s->sync_wq, s->pending || kthread_should_stop());

		if (kthread_should_stop())
			break;

		spin_lock_irqsave(&s->lock, flags);
		s->pending = false;
		src_cpu = s->src_cpu;
		spin_unlock_irqrestore(&s->lock, flags);

		ret = cpufreq_get_policy(&src_policy, src_cpu);
		if (ret)
			continue;

		ret = cpufreq_get_policy(&dest_policy, dest_cpu);
		if (ret)
			continue;

		req_freq = load_based_syncs ?
			(dest_policy.cpuinfo.max_freq * s->task_load) / 100 :
							src_policy.cur;

		if (req_freq <= dest_policy.cpuinfo.min_freq) {
			pr_debug("No sync. Sync Freq:%u\n", req_freq);
			continue;
		}

		if (sync_threshold)
			req_freq = min(sync_threshold, req_freq);

		cancel_delayed_work_sync(&s->boost_rem);

		s->boost_min = req_freq;

		/* Force policy re-evaluation to trigger adjust notifier. */
		get_online_cpus();
		if (cpu_online(src_cpu))
			/*
			 * Send an unchanged policy update to the source
			 * CPU. Even though the policy isn't changed from
			 * its existing boosted or non-boosted state
			 * notifying the source CPU will let the governor
			 * know a boost happened on another CPU and that it
			 * should re-evaluate the frequency at the next timer
			 * event without interference from a min sample time.
			 */
			cpufreq_update_policy(src_cpu);
		if (cpu_online(dest_cpu)) {
			cpufreq_update_policy(dest_cpu);
			queue_delayed_work_on(dest_cpu, cpu_boost_wq,
				&s->boost_rem, msecs_to_jiffies(boost_ms));
		} else {
			s->boost_min = 0;
		}
		put_online_cpus();
	}

	return 0;
}

#ifndef CONFIG_SCHED_BFS
static int boost_migration_notify(struct notifier_block *nb,
				unsigned long unused, void *arg)
{
	struct migration_notify_data *mnd = arg;
	unsigned long flags;
	struct cpu_sync *s = &per_cpu(sync_info, mnd->dest_cpu);

	if (load_based_syncs && (mnd->load <= migration_load_threshold))
		return NOTIFY_OK;

	if (load_based_syncs && ((mnd->load < 0) || (mnd->load > 100))) {
		pr_err("cpu-boost:Invalid load: %d\n", mnd->load);
		return NOTIFY_OK;
	}

	if (!load_based_syncs && (mnd->src_cpu == mnd->dest_cpu))
		return NOTIFY_OK;

	if (!boost_ms)
		return NOTIFY_OK;

	/* Avoid deadlock in try_to_wake_up() */
	if (s->thread == current)
		return NOTIFY_OK;

	pr_debug("Migration: CPU%d --> CPU%d\n", mnd->src_cpu, mnd->dest_cpu);
	spin_lock_irqsave(&s->lock, flags);
	s->pending = true;
	s->src_cpu = mnd->src_cpu;
	s->task_load = load_based_syncs ? mnd->load : 0;
	spin_unlock_irqrestore(&s->lock, flags);
	wake_up(&s->sync_wq);

	return NOTIFY_OK;
}

static struct notifier_block boost_migration_nb = {
	.notifier_call = boost_migration_notify,
};
#endif

static int cpu_boost_init(void)
{
	int cpu;
	struct cpu_sync *s;

	cpufreq_register_notifier(&boost_adjust_nb, CPUFREQ_POLICY_NOTIFIER);

	cpu_boost_wq = alloc_workqueue("cpuboost_wq", WQ_HIGHPRI, 0);
	if (!cpu_boost_wq)
		return -EFAULT;

	for_each_possible_cpu(cpu) {
		s = &per_cpu(sync_info, cpu);
		s->cpu = cpu;
		init_waitqueue_head(&s->sync_wq);
		spin_lock_init(&s->lock);
		INIT_DELAYED_WORK(&s->boost_rem, do_boost_rem);
		s->thread = kthread_run(boost_mig_sync_thread, (void *)cpu,
					"boost_sync/%d", cpu);
		set_cpus_allowed(s->thread, *cpumask_of(cpu));
	}
#ifndef CONFIG_SCHED_BFS
	atomic_notifier_chain_register(&migration_notifier_head,
					&boost_migration_nb);
#endif

	return 0;
}
late_initcall(cpu_boost_init);
