/*
 *  cpuidle-powernv - idle state cpuidle driver.
 *  Adapted from drivers/cpuidle/cpuidle-pseries
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/cpuidle.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/clockchips.h>

#include <asm/machdep.h>
#include <asm/firmware.h>
#include <asm/runlatch.h>

/* Flags and constants used in PowerNV platform */

#define MAX_POWERNV_IDLE_STATES	8
#define IDLE_USE_INST_NAP	0x00010000 /* Use nap instruction */
#define IDLE_USE_INST_SLEEP	0x00020000 /* Use sleep instruction */

struct cpuidle_driver powernv_idle_driver = {
	.name             = "powernv_idle",
	.owner            = THIS_MODULE,
};

static int max_idle_state;
static struct cpuidle_state *cpuidle_state_table;

static u64 stop_psscr_table[CPUIDLE_STATE_MAX];

static u64 default_snooze_timeout;
static bool snooze_timeout_en;

static u64 get_snooze_timeout(struct cpuidle_device *dev,
			      struct cpuidle_driver *drv,
			      int index)
{
	int i;

	if (unlikely(!snooze_timeout_en))
		return default_snooze_timeout;

	for (i = index + 1; i < drv->state_count; i++) {
		struct cpuidle_state *s = &drv->states[i];
		struct cpuidle_state_usage *su = &dev->states_usage[i];

		if (s->disabled || su->disable)
			continue;

		return s->target_residency * tb_ticks_per_usec;
	}

	return default_snooze_timeout;
}

static int snooze_loop(struct cpuidle_device *dev,
			struct cpuidle_driver *drv,
			int index)
{
	local_irq_enable();
	set_thread_flag(TIF_POLLING_NRFLAG);

	snooze_exit_time = get_tb() + get_snooze_timeout(dev, drv, index);
	ppc64_runlatch_off();
	while (!need_resched()) {
		HMT_low();
		HMT_very_low();
	}

	HMT_medium();
	ppc64_runlatch_on();
	clear_thread_flag(TIF_POLLING_NRFLAG);
	smp_mb();
	return index;
}

static int nap_loop(struct cpuidle_device *dev,
			struct cpuidle_driver *drv,
			int index)
{
	ppc64_runlatch_off();
	power7_idle();
	ppc64_runlatch_on();
	return index;
}

static int fastsleep_loop(struct cpuidle_device *dev,
				struct cpuidle_driver *drv,
				int index)
{
	unsigned long old_lpcr = mfspr(SPRN_LPCR);
	unsigned long new_lpcr;

	if (unlikely(system_state < SYSTEM_RUNNING))
		return index;

	new_lpcr = old_lpcr;
	new_lpcr &= ~(LPCR_MER | LPCR_PECE); /* lpcr[mer] must be 0 */

	/* exit powersave upon external interrupt, but not decrementer
	 * interrupt.
	 */
	new_lpcr |= LPCR_PECE0;

	mtspr(SPRN_LPCR, new_lpcr);
	power7_sleep();

	mtspr(SPRN_LPCR, old_lpcr);

	return index;
}

/*
 * States for dedicated partition case.
 */
static struct cpuidle_state powernv_states[MAX_POWERNV_IDLE_STATES] = {
	{ /* Snooze */
		.name = "snooze",
		.desc = "snooze",
		.flags = CPUIDLE_FLAG_TIME_VALID,
		.exit_latency = 0,
		.target_residency = 0,
		.enter = &snooze_loop },
	{ /* NAP */
		.name = "NAP",
		.desc = "NAP",
		.flags = CPUIDLE_FLAG_TIME_VALID,
		.exit_latency = 10,
		.target_residency = 100,
		.enter = &nap_loop },
	 { /* Fastsleep */
		.name = "fastsleep",
		.desc = "fastsleep",
		.flags = CPUIDLE_FLAG_TIME_VALID,
		.exit_latency = 10,
		.target_residency = 100,
		.enter = &fastsleep_loop },
};

static int powernv_cpuidle_add_cpu_notifier(struct notifier_block *n,
			unsigned long action, void *hcpu)
{
	int hotcpu = (unsigned long)hcpu;
	struct cpuidle_device *dev =
				per_cpu(cpuidle_devices, hotcpu);

	if (dev && cpuidle_get_driver()) {
		switch (action) {
		case CPU_ONLINE:
		case CPU_ONLINE_FROZEN:
			cpuidle_pause_and_lock();
			cpuidle_enable_device(dev);
			cpuidle_resume_and_unlock();
			break;

		case CPU_DEAD:
		case CPU_DEAD_FROZEN:
			cpuidle_pause_and_lock();
			cpuidle_disable_device(dev);
			cpuidle_resume_and_unlock();
			break;

		default:
			return NOTIFY_DONE;
		}
	}
	return NOTIFY_OK;
}

static struct notifier_block setup_hotplug_notifier = {
	.notifier_call = powernv_cpuidle_add_cpu_notifier,
};

/*
 * powernv_cpuidle_driver_init()
 */
static int powernv_cpuidle_driver_init(void)
{
	int idle_state;
	struct cpuidle_driver *drv = &powernv_idle_driver;

	drv->state_count = 0;

	for (idle_state = 0; idle_state < max_idle_state; ++idle_state) {
		/* Is the state not enabled? */
		if (cpuidle_state_table[idle_state].enter == NULL)
			continue;

		drv->states[drv->state_count] =	/* structure copy */
			cpuidle_state_table[idle_state];

		drv->state_count += 1;
	}

	/*
	 * On the PowerNV platform cpu_present may be less than cpu_possible in
	 * cases when firmware detects the CPU, but it is not available to the
	 * OS.  If CONFIG_HOTPLUG_CPU=n, then such CPUs are not hotplugable at
	 * run time and hence cpu_devices are not created for those CPUs by the
	 * generic topology_init().
	 *
	 * drv->cpumask defaults to cpu_possible_mask in
	 * __cpuidle_driver_init().  This breaks cpuidle on PowerNV where
	 * cpu_devices are not created for CPUs in cpu_possible_mask that
	 * cannot be hot-added later at run time.
	 *
	 * Trying cpuidle_register_device() on a CPU without a cpu_device is
	 * incorrect, so pass a correct CPU mask to the generic cpuidle driver.
	 */

	drv->cpumask = (struct cpumask *)cpu_present_mask;

	return 0;
}

static int powernv_add_idle_states(void)
{
	struct device_node *power_mgt;
	int nr_idle_states = 1; /* Snooze */
	int dt_idle_states;
	const __be32 *idle_state_flags;
	const __be32 *idle_state_latency;
	u32 len_flags, flags, latency_ns;
	int i;

	/* Currently we have snooze statically defined */

	power_mgt = of_find_node_by_path("/ibm,opal/power-mgt");
	if (!power_mgt) {
		pr_warn("opal: PowerMgmt Node not found\n");
		return nr_idle_states;
	}

	idle_state_flags = of_get_property(power_mgt, "ibm,cpu-idle-state-flags", &len_flags);
	if (!idle_state_flags) {
		pr_warn("DT-PowerMgmt: missing ibm,cpu-idle-state-flags\n");
		return nr_idle_states;
	}

	idle_state_latency = of_get_property(power_mgt,
			"ibm,cpu-idle-state-latencies-ns", NULL);
	if (!idle_state_latency) {
		pr_warn("DT-PowerMgmt: missing ibm,cpu-idle-state-latencies-ns\n");
		return nr_idle_states;
	}

	dt_idle_states = len_flags / sizeof(u32);

	for (i = 0; i < dt_idle_states; i++) {

		flags = be32_to_cpu(idle_state_flags[i]);

		/* Cpuidle accepts exit_latency in us and we estimate
		 * target residency to be 10x exit_latency
		 */
		latency_ns = be32_to_cpu(idle_state_latency[i]);
		if (flags & IDLE_USE_INST_NAP) {
			/* Add NAP state */
			strcpy(powernv_states[nr_idle_states].name, "Nap");
			strcpy(powernv_states[nr_idle_states].desc, "Nap");
			powernv_states[nr_idle_states].flags = CPUIDLE_FLAG_TIME_VALID;
			powernv_states[nr_idle_states].exit_latency =
					((unsigned int)latency_ns) / 1000;
			powernv_states[nr_idle_states].target_residency =
					((unsigned int)latency_ns / 100);
			powernv_states[nr_idle_states].enter = &nap_loop;
			nr_idle_states++;
		}

		if (flags & IDLE_USE_INST_SLEEP) {
			/* Add FASTSLEEP state */
			strcpy(powernv_states[nr_idle_states].name, "FastSleep");
			strcpy(powernv_states[nr_idle_states].desc, "FastSleep");
			powernv_states[nr_idle_states].flags =
				CPUIDLE_FLAG_TIME_VALID | CPUIDLE_FLAG_TIMER_STOP;
			powernv_states[nr_idle_states].exit_latency =
					((unsigned int)latency_ns) / 1000;
			powernv_states[nr_idle_states].target_residency =
					((unsigned int)latency_ns / 100);
			powernv_states[nr_idle_states].enter = &fastsleep_loop;
			nr_idle_states++;
		}
	}

	return nr_idle_states;
}

/*
 * powernv_idle_probe()
 * Choose state table for shared versus dedicated partition
 */
static int powernv_idle_probe(void)
{
	if (cpuidle_disable != IDLE_NO_OVERRIDE)
		return -ENODEV;

	if (firmware_has_feature(FW_FEATURE_OPALv3)) {
		cpuidle_state_table = powernv_states;
		/* Device tree can indicate more idle states */
		max_idle_state = powernv_add_idle_states();
		default_snooze_timeout = TICK_USEC * tb_ticks_per_usec;
		if (max_idle_state > 1)
			snooze_timeout_en = true;
 	} else
 		return -ENODEV;

	return 0;
}

static int __init powernv_processor_idle_init(void)
{
	int retval;

	retval = powernv_idle_probe();
	if (retval)
		return retval;

	powernv_cpuidle_driver_init();
	retval = cpuidle_register(&powernv_idle_driver, NULL);
	if (retval) {
		printk(KERN_DEBUG "Registration of powernv driver failed.\n");
		return retval;
	}

	register_cpu_notifier(&setup_hotplug_notifier);
	printk(KERN_DEBUG "powernv_idle_driver registered\n");
	return 0;
}

device_initcall(powernv_processor_idle_init);
