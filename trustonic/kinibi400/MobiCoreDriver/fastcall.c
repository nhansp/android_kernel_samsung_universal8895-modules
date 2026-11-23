/*
 * Copyright (c) 2013-2018 TRUSTONIC LIMITED
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/moduleparam.h>
#include <linux/debugfs.h>
#include <linux/sched.h>	/* local_clock */
#include <linux/version.h>
#include <linux/sched/clock.h>	/* local_clock */
#include <linux/uaccess.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/arm-smccc.h>

#include "public/mc_user.h"
#include "public/mc_linux_api.h"

#include "mci/mcifc.h"

#include "platform.h"	/* MC_FASTCALL_WORKER_THREAD and more */
#include "main.h"
#include "clock.h"	/* mc_clock_enable, mc_clock_disable */
#include "fastcall.h"

/* ExySp: Lock for core switch processing */
static DEFINE_MUTEX(core_switch_lock);
static u8 core_status = 0xFF;
#ifdef CONFIG_SECURE_OS_BOOSTER_API
static int disable_local_timer;
int mc_timer(void);
void mc_set_schedule_policy(int core);
#endif
int __mc_switch_core(int cpu);

/* The fastcall ABI of the Kinibi 400 secure world */
static_assert(MC_FC_INIT == 0xFF000001);
static_assert(MC_FC_INFO == 0xFF000002);
static_assert(MC_FC_MEM_TRACE == 0xFF00000A);
static_assert(MC_FC_SWAP_CPU == 0xFF000036);
static_assert(MC_SMC_N_YIELD == 3);
static_assert(MC_SMC_N_SIQ == 4);

struct fastcall_work {
	struct kthread_work work;
	void *data;
};

/* generic fast call parameters */
union mc_fc_generic {
	struct mc_fc_as_in {
		u32 cmd;
		u32 param[3];
	} as_in;
	struct {
		u32 resp;
		u32 ret;
		u32 param[2];
	} as_out;
};

/* fast call init */
union mc_fc_init {
	union mc_fc_generic as_generic;
	struct {
		u32 cmd;
		u32 base;
		u32 nq_info;
		u32 mcp_info;
	} as_in;
	struct {
		u32 resp;
		u32 ret;
		u32 flags;
		u32 rfu;
	} as_out;
};

/* fast call info parameters */
union mc_fc_info {
	union mc_fc_generic as_generic;
	struct {
		u32 cmd;
		u32 ext_info_id;
		u32 rfu[2];
	} as_in;
	struct {
		u32 resp;
		u32 ret;
		u32 state;
		u32 ext_info;
	} as_out;
};

/* fast call switch Core parameters */
union mc_fc_swich_core {
	union mc_fc_generic as_generic;
	struct {
		u32 cmd;
		u32 core_id;
		u32 rfu[2];
	} as_in;
	struct {
		u32 resp;
		u32 ret;
		u32 state;
		u32 ext_info;
	} as_out;
};

static struct kthread_worker *fastcall_worker;
static int tee_cpuhp_state;

/* Structure to log SMC calls */
struct smc_log_entry {
	u64 cpu_clk;
	struct mc_fc_as_in as_in;
};

#define SMC_LOG_SIZE 1024
static struct smc_log_entry smc_log[SMC_LOG_SIZE];
static int smc_log_index;

/*
 * _smc() - fast call to MobiCore
 *
 * @data: pointer to fast call data
 */
static inline int _smc(union mc_fc_generic *mc_fc_generic)
{
	if (!mc_fc_generic)
		return -EINVAL;

	/* Log SMC call */
	smc_log[smc_log_index].cpu_clk = local_clock();
	smc_log[smc_log_index].as_in = mc_fc_generic->as_in;
	if (++smc_log_index >= SMC_LOG_SIZE)
		smc_log_index = 0;

	{
		struct arm_smccc_res res;

		arm_smccc_smc(mc_fc_generic->as_in.cmd,
			      mc_fc_generic->as_in.param[0],
			      mc_fc_generic->as_in.param[1],
			      mc_fc_generic->as_in.param[2], 0, 0, 0, 0, &res);

		/* set response */
		mc_fc_generic->as_out.resp     = res.a0;
		mc_fc_generic->as_out.ret      = res.a1;
		mc_fc_generic->as_out.param[0] = res.a2;
		mc_fc_generic->as_out.param[1] = res.a3;
	}
	return 0;
}

static int active_cpu;

#ifdef CONFIG_SECURE_OS_BOOSTER_API
/* ExySp: for sos performance */
void mc_set_schedule_policy(int core)
{
	if (core == DEFAULT_BIG_CORE)
		disable_local_timer = 1;
	else
		disable_local_timer = 0;

	return;
}
#else
static inline void mc_set_schedule_policy(int core)
{
}
#endif

/* ExySp: for sos performance */
static int mc_cpu_offline(int cpu)
{
	int i;

	mutex_lock(&core_switch_lock);
	core_status &= ~(0x1 << cpu);
	if (active_cpu != cpu) {
		mc_dev_devel("not active CPU, no action taken");
		mutex_unlock(&core_switch_lock);
		return 0;
	}

	/* Chose the first online CPU and switch! */
	for_each_online_cpu(i) {
		if (cpu != i) {
			mc_dev_devel("CPU %d is dying, switching to %d",
				     cpu, i);
			mc_set_schedule_policy(DEFAULT_LITTLE_CORE);
			__mc_switch_core(i);
			break;
		}

		mc_dev_devel("Skipping CPU %d", cpu);
	}
	mutex_unlock(&core_switch_lock);
	return 0;
}

/* ExySp: for sos performance */
static void mc_cpu_online(int cpu)
{
	mutex_lock(&core_switch_lock);
	core_status |= (0x1 << cpu);

	if (cpu == NONBOOT_LITTLE_CORE)
		__mc_switch_core(NONBOOT_LITTLE_CORE);
	mutex_unlock(&core_switch_lock);
}

static int tee_cpu_online(unsigned int cpu)
{
	mc_cpu_online(cpu);
	return 0;
}

static int nq_cpu_down_prep(unsigned int cpu)
{
	mc_dev_info("CPU #%d is going to die", cpu);
	return mc_cpu_offline(cpu);
}

static cpumask_t mc_exec_core_switch(union mc_fc_generic *mc_fc_generic)
{
	cpumask_t cpu;
	int new_cpu;
	u32 cpu_id[] = CPU_IDS;

	new_cpu = mc_fc_generic->as_in.param[0];
	mc_fc_generic->as_in.param[0] = cpu_id[mc_fc_generic->as_in.param[0]];

	if (_smc(mc_fc_generic) != 0 || mc_fc_generic->as_out.ret != 0) {
		/* ExySp: print info */
		mc_dev_info("CoreSwap failed %d -> %d (cpu %d still active)",
			     raw_smp_processor_id(),
			     mc_fc_generic->as_in.param[0],
			     raw_smp_processor_id());
	} else {
		active_cpu = new_cpu;
		/* ExySp: print info */
		mc_dev_info("CoreSwap ok %d -> %d",
			     raw_smp_processor_id(), active_cpu);
	}
	cpumask_clear(&cpu);
	cpumask_set_cpu(active_cpu, &cpu);
	return cpu;
}

static ssize_t debug_coreswitch_write(struct file *file,
				      const char __user *buffer,
				      size_t buffer_len, loff_t *x)
{
	int new_cpu = 0;

	/* Invalid data, nothing to do */
	if (buffer_len < 1)
		return -EINVAL;

	if (kstrtoint_from_user(buffer, buffer_len, 0, &new_cpu))
		return -EINVAL;

	mc_dev_devel("Set active cpu to %d", new_cpu);
	mc_switch_core(new_cpu);
	return buffer_len;
}

static ssize_t debug_coreswitch_read(struct file *file, char __user *buffer,
				     size_t buffer_len, loff_t *ppos)
{
	char cpu_str[8];
	int ret = 0;

	ret = snprintf(cpu_str, sizeof(cpu_str), "%d\n", mc_active_core());
	if (ret < 0)
		return -EINVAL;

	return simple_read_from_buffer(buffer, buffer_len, ppos,
				       cpu_str, ret);
}

static const struct file_operations mc_debug_coreswitch_ops = {
	.write = debug_coreswitch_write,
	.read = debug_coreswitch_read,
};

static void fastcall_work_func(struct kthread_work *work)
{
	struct fastcall_work *fc_work =
		container_of(work, struct fastcall_work, work);
	union mc_fc_generic *mc_fc_generic = fc_work->data;

	if (!mc_fc_generic)
		return;

	mc_clock_enable();

	if (mc_fc_generic->as_in.cmd == MC_FC_SWAP_CPU) {
		cpumask_t new_msk = mc_exec_core_switch(mc_fc_generic);

		/*
		 * ExySp: follow the SWd. Stay a per-cpu kthread, which may still
		 * run on a core that is going offline.
		 */
		set_cpus_allowed_ptr(current, &new_msk);
		kthread_set_per_cpu(current, cpumask_first(&new_msk));
	} else {
		/* ExySp: for sos performance */
#ifdef CONFIG_SECURE_OS_BOOSTER_API
		if (active_cpu == DEFAULT_BIG_CORE && disable_local_timer)
			mc_timer();
#endif
		_smc(mc_fc_generic);
	}

	mc_clock_disable();
}

static bool mc_fastcall(void *data)
{
	struct fastcall_work fc_work = {
		KTHREAD_WORK_INIT(fc_work.work, fastcall_work_func),
		.data = data,
	};

	if (!kthread_queue_work(fastcall_worker, &fc_work.work))
		return false;

	/* If work is queued or executing, wait for it to finish execution */
	kthread_flush_work(&fc_work.work);
	return true;
}

int mc_fastcall_init(void)
{
	int ret = mc_clock_init();

	if (ret)
		return ret;

	fastcall_worker = kthread_create_worker_on_cpu(0, 0, "tee_fastcall");
	if (IS_ERR(fastcall_worker)) {
		ret = PTR_ERR(fastcall_worker);
		fastcall_worker = NULL;
		mc_dev_err("cannot create fastcall wq: %d", ret);
		return ret;
	}

	set_user_nice(fastcall_worker->task, MIN_NICE);
	kthread_set_per_cpu(fastcall_worker->task, 0);
	wake_up_process(fastcall_worker->task);

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"tee/trustonic:online",
					tee_cpu_online, nq_cpu_down_prep);
	if (ret < 0) {
		mc_dev_err("cpu online callback setup failed: %d", ret);
		kthread_destroy_worker(fastcall_worker);
		fastcall_worker = NULL;
		return ret;
	}
	tee_cpuhp_state = ret;

	/* Create debugfs structs entry */
	debugfs_create_file("active_cpu", 0600, g_ctx.debug_dir, NULL,
			    &mc_debug_coreswitch_ops);

	return 0;
}

void mc_fastcall_exit(void)
{
	if (fastcall_worker) {
		cpuhp_remove_state_nocalls(tee_cpuhp_state);
		kthread_destroy_worker(fastcall_worker);
		fastcall_worker = NULL;
	}
	mc_clock_exit();
}

/*
 * convert fast call return code to linux driver module error code
 */
static int convert_fc_ret(u32 ret)
{
	switch (ret) {
	case MC_FC_RET_OK:
		return 0;
	case MC_FC_RET_ERR_INVALID:
		return -EINVAL;
	case MC_FC_RET_ERR_ALREADY_INITIALIZED:
		return -EBUSY;
	default:
		return -EFAULT;
	}
}

int mc_fc_init(uintptr_t base_pa, ptrdiff_t off, size_t q_len, size_t buf_len)
{
	u32 base_high = (u32)(base_pa >> 32);
	union mc_fc_init fc_init;

	/* Call the INIT fastcall to setup MobiCore initialization */
	memset(&fc_init, 0, sizeof(fc_init));
	fc_init.as_in.cmd = MC_FC_INIT;
	/* base address of mci buffer PAGE_SIZE (default is 4KB) aligned */
	fc_init.as_in.base = (u32)base_pa;
	/* notification buffer start/length [16:16] [start, length] */
	fc_init.as_in.nq_info =
	    (u32)(((base_high & 0xFFFF) << 16) | (q_len & 0xFFFF));
	/* mcp buffer start/length [16:16] [start, length] */
	fc_init.as_in.mcp_info = (u32)((off << 16) | (buf_len & 0xFFFF));
	mc_dev_devel("cmd=%d, base=0x%08x,nq_info=0x%08x, mcp_info=0x%08x",
		     fc_init.as_in.cmd, fc_init.as_in.base,
		     fc_init.as_in.nq_info, fc_init.as_in.mcp_info);
	mc_fastcall(&fc_init.as_generic);
	mc_dev_devel("out cmd=0x%08x, ret=0x%08x", fc_init.as_out.resp,
		     fc_init.as_out.ret);
	if (fc_init.as_out.flags & MC_FC_INIT_FLAG_LPAE)
		g_ctx.f_lpae = true;

	return convert_fc_ret(fc_init.as_out.ret);
}

int mc_fc_info(u32 ext_info_id, u32 *state, u32 *ext_info)
{
	union mc_fc_info fc_info;
	int ret = 0;

	memset(&fc_info, 0, sizeof(fc_info));
	fc_info.as_in.cmd = MC_FC_INFO;
	fc_info.as_in.ext_info_id = ext_info_id;
	mc_fastcall(&fc_info.as_generic);
	ret = convert_fc_ret(fc_info.as_out.ret);
	if (ret) {
		if (state)
			*state = MC_STATUS_NOT_INITIALIZED;

		if (ext_info)
			*ext_info = 0;

		mc_dev_err("code %d for idx %d", ret, ext_info_id);
	} else {
		if (state)
			*state = fc_info.as_out.state;

		if (ext_info)
			*ext_info = fc_info.as_out.ext_info;
	}

	return ret;
}

int mc_fc_mem_trace(phys_addr_t buffer, u32 size)
{
	union mc_fc_generic mc_fc_generic;

	memset(&mc_fc_generic, 0, sizeof(mc_fc_generic));
	mc_fc_generic.as_in.cmd = MC_FC_MEM_TRACE;
	mc_fc_generic.as_in.param[0] = (u32)buffer;
	mc_fc_generic.as_in.param[1] = (u32)(buffer >> 32);
	mc_fc_generic.as_in.param[2] = size;
	mc_fastcall(&mc_fc_generic);
	return convert_fc_ret(mc_fc_generic.as_out.ret);
}

int mc_fc_nsiq(void)
{
	union mc_fc_generic fc;
	int ret;

	memset(&fc, 0, sizeof(fc));
	fc.as_in.cmd = MC_SMC_N_SIQ;
	mc_fastcall(&fc);
	ret = convert_fc_ret(fc.as_out.ret);
	if (ret)
		mc_dev_err("failed: %d", ret);

	return ret;
}

int mc_fc_yield(void)
{
	union mc_fc_generic fc;
	int ret;

	memset(&fc, 0, sizeof(fc));
	fc.as_in.cmd = MC_SMC_N_YIELD;
	mc_fastcall(&fc);
	ret = convert_fc_ret(fc.as_out.ret);
	if (ret)
		mc_dev_err("failed: %d", ret);

	return ret;
}

static int show_smc_log_entry(struct kasnprintf_buf *buf,
			      struct smc_log_entry *entry)
{
	return kasnprintf(buf, "%20llu %10d 0x%08x 0x%08x 0x%08x\n",
			  entry->cpu_clk, (s32)entry->as_in.cmd,
			  entry->as_in.param[0], entry->as_in.param[1],
			  entry->as_in.param[2]);
}

/*
 * Dump SMC log circular buffer, starting from oldest command. It is assumed
 * nothing goes in any more at this point.
 */
int mc_fastcall_debug_smclog(struct kasnprintf_buf *buf)
{
	int i, ret = 0;

	ret = kasnprintf(buf, "%20s %10s %-10s %-10s %-10s\n",
			 "CPU clock", "command", "param1", "param2", "param3");
	if (ret < 0)
		return ret;

	if (smc_log[smc_log_index].cpu_clk)
		/* Buffer has wrapped around, dump end (oldest records) */
		for (i = smc_log_index; i < SMC_LOG_SIZE; i++) {
			ret = show_smc_log_entry(buf, &smc_log[i]);
			if (ret < 0)
				return ret;
		}

	/* Dump first records */
	for (i = 0; i < smc_log_index; i++) {
		ret = show_smc_log_entry(buf, &smc_log[i]);
		if (ret < 0)
			return ret;
	}

	return ret;
}

int mc_active_core(void)
{
	return active_cpu;
}

/* ExySp: for sos performance; caller holds core_switch_lock */
int __mc_switch_core(int cpu)
{
	s32 ret = 0;
	union mc_fc_swich_core fc_switch_core;

	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	if (!cpu_online(cpu))
		return 1;

	memset(&fc_switch_core, 0, sizeof(fc_switch_core));
	fc_switch_core.as_in.cmd = MC_FC_SWAP_CPU;
	if (cpu < COUNT_OF_CPUS)
		fc_switch_core.as_in.core_id = cpu;
	else
		fc_switch_core.as_in.core_id = 0;

	mc_dev_devel("<- cmd=0x%08x, core_id=0x%08x",
		     fc_switch_core.as_in.cmd, fc_switch_core.as_in.core_id);
	/* ExySp: for sos performance */
	mc_dev_info("<- cpu=0x%08x, active_cpu=0x%08x\n",
		     cpu, active_cpu);
	mc_fastcall(&fc_switch_core.as_generic);
	ret = convert_fc_ret(fc_switch_core.as_out.ret);
	mc_dev_devel("exit with %d/0x%08X", ret, ret);
	return ret;
}

/* ExySp: for sos performance */
int mc_switch_core(int cpu)
{
       int ret;
       mutex_lock(&core_switch_lock);
       if (!(core_status & (0x1 << cpu))){
               mc_dev_devel("Core status... core #%d is off line\n", cpu);
               mutex_unlock(&core_switch_lock);
               return 1;
       }
       ret = __mc_switch_core(cpu);
       mutex_unlock(&core_switch_lock);
       return ret;
}
