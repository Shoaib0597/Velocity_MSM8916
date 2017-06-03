/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 * Copyright (c) 2015 Francisco Franco
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_tsens.h>
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/hrtimer.h>

/* Temp Threshold is the LOWEST level to start throttling. */
#define _temp_threshold		70
/* This is the modifier for each of the progressive throttle levels to kick in*/
#define _temp_step			4
/* Default the poll interval to 1 second. Time in usecs */
#define _poll_interval		1000000

int ENABLED = 1;
int TEMP_THRESHOLD = _temp_threshold;
int TEMP_STEP = _temp_step;
int LEVEL_VERY_HOT = _temp_threshold + _temp_step;
int LEVEL_HOT = _temp_threshold + (_temp_step * 2);
int LEVEL_HELL = _temp_threshold + (_temp_step * 3);
int FREQ_HELL = 960000;
int FREQ_VERY_HOT = 1267200;
int FREQ_HOT = 1728000;
int FREQ_WARM = 2265600;
int POLL_INTERVAL = _poll_interval;

// allow full frequency mitigation
bool full_fm = false;
module_param(full_fm, bool, 0644);

/* SYSFS */

/* Enable toggle */
static int set_enabled(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	int i;

	ret = kstrtouint(val, 10, &i);
	if (ret)
		return -EINVAL;
	if (i < 0 || i > 1)
		return -EINVAL;
		
	ret = param_set_int(val, kp);

	return ret;
}

static struct kernel_param_ops enabled_ops = {
	.set = set_enabled,
	.get = param_get_int,
};

module_param_cb(enabled, &enabled_ops, &ENABLED, 0644);

/* Temperature Threshold Storage */
static int set_temp_threshold(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	int i;

	ret = kstrtouint(val, 10, &i);
	if (ret)
		return -EINVAL;
	if (i < 40 || i > 90)
		return -EINVAL;
	
	LEVEL_VERY_HOT = i + TEMP_STEP;
	LEVEL_HOT = i + (TEMP_STEP * 2);
	LEVEL_HELL = i + (TEMP_STEP * 3);
	
	ret = param_set_int(val, kp);

	return ret;
}

static struct kernel_param_ops temp_threshold_ops = {
	.set = set_temp_threshold,
	.get = param_get_int,
};

module_param_cb(temp_threshold, &temp_threshold_ops, &TEMP_THRESHOLD, 0644);

/* Temperature Step Storage */
static int set_temp_step(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	int i;

	ret = kstrtouint(val, 10, &i);
	if (ret)
		return -EINVAL;
	/*	Restrict the values to 1-6 as this will result in threshold + value - value *3
		without a restriction this could result in significanty higher than expected values*/
	if (i < 1 || i > 6)
		return -EINVAL;
	
	LEVEL_VERY_HOT = TEMP_THRESHOLD + i;
	LEVEL_HOT = TEMP_THRESHOLD + (i * 2);
	LEVEL_HELL = TEMP_THRESHOLD + (i * 3);
	
	ret = param_set_int(val, kp);

	return ret;
}

static struct kernel_param_ops temp_step_ops = {
	.set = set_temp_step,
	.get = param_get_int,
};

module_param_cb(temp_step, &temp_step_ops, &TEMP_STEP, 0644);

/* Poll interval Storage */
static int set_poll_interval(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	int i;

	ret = kstrtouint(val, 10, &i);
	if (ret)
		return -EINVAL;
	/*	Restrict the values to between 250ms and 3 seconds */
	if (i < 250000 || i > 3000000)
		return -EINVAL;
	
	ret = param_set_int(val, kp);

	return ret;
}

static struct kernel_param_ops poll_interval_ops = {
	.set = set_poll_interval,
	.get = param_get_int,
};

module_param_cb(poll_interval, &poll_interval_ops, &POLL_INTERVAL, 0644);


/* Frequency limit storage */
static int set_freq_limit(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	int i, cnt;
	int valid = 0;
	struct cpufreq_policy *policy;
	static struct cpufreq_frequency_table *tbl = NULL;
	
	ret = kstrtouint(val, 10, &i);
	if (ret)
		return -EINVAL;

	/* Verify that the value being set is an actual frequency 
	 * easiest way I could think of was to just loop through the table
	 * and check if the value was the same */
	policy = cpufreq_cpu_get(0);
	tbl = cpufreq_frequency_get_table(0);
	for (cnt = 0; (tbl[cnt].frequency != CPUFREQ_TABLE_END); cnt++) {
		if (cnt > 0)
			if (tbl[cnt].frequency == i)
				valid = 1;
	}
	/* If value being set wasn't found in the frequency table, valid will equal 0 */
	if (!valid)
		return -EINVAL;
	
	/* Perform some sanity checks on the values that we're storing 
	 * to make sure that they're scaling linearly 					*/
	if (strcmp( kp->name, "msm_thermal.freq_warm") == 0 && i <= FREQ_HOT) 
		return -EINVAL;
	if ( strcmp( kp->name, "msm_thermal.freq_hot") == 0 &&  ( i >= FREQ_WARM || i <= FREQ_VERY_HOT ))
		return -EINVAL;	
	if ( strcmp( kp->name, "msm_thermal.freq_very_hot") == 0 && ( i >= FREQ_HOT || i <= FREQ_HELL ))
		return -EINVAL;		
	if ( strcmp( kp->name, "msm_thermal.freq_hell") == 0 && i >= FREQ_VERY_HOT ) 
		return -EINVAL;		
	/* End Sanity Checks */
	
	ret = param_set_int(val, kp);

	return ret;
}

static struct kernel_param_ops freq_limit_ops = {
	.set = set_freq_limit,
	.get = param_get_int,
};

module_param_cb(freq_hell, &freq_limit_ops, &FREQ_HELL, 0644);
module_param_cb(freq_very_hot, &freq_limit_ops, &FREQ_VERY_HOT, 0644);
module_param_cb(freq_hot, &freq_limit_ops, &FREQ_HOT, 0644);
module_param_cb(freq_warm, &freq_limit_ops, &FREQ_WARM, 0644);

/* SYSFS END */

static struct thermal_info {
	uint32_t cpuinfo_max_freq;
	uint32_t limited_max_freq;
	unsigned int safe_diff;
	bool throttling;
	bool pending_change;
	u64 limit_cpu_time;
} info = {
	.cpuinfo_max_freq = LONG_MAX,
	.limited_max_freq = LONG_MAX,
	.safe_diff = 5,
	.throttling = false,
	.pending_change = false,
};

static struct msm_thermal_data msm_thermal_info;

static struct delayed_work check_temp_work;

static int msm_thermal_cpufreq_callback(struct notifier_block *nfb,
		unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;

	if (event != CPUFREQ_ADJUST && !info.pending_change)
		return 0;

	cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq,
		info.limited_max_freq);

	return 0;
}

static struct notifier_block msm_thermal_cpufreq_notifier = {
	.notifier_call = msm_thermal_cpufreq_callback,
};

static void limit_cpu_freqs(uint32_t max_freq)
{
	unsigned int cpu;

	if (info.limited_max_freq == max_freq)
		return;

	info.limited_max_freq = max_freq;
	info.pending_change = true;
	info.limit_cpu_time = ktime_to_us(ktime_get());

	get_online_cpus();
	for_each_online_cpu(cpu) {
		cpufreq_update_policy(cpu);
		pr_info("%s: Setting cpu%d max frequency to %d\n",
				KBUILD_MODNAME, cpu, info.limited_max_freq);
	}
	put_online_cpus();

	info.pending_change = false;
}

static void check_temp(struct work_struct *work)
{
	struct tsens_device tsens_dev;
	uint32_t freq = 0;
	long temp = 0;
	u64 now;

	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	tsens_get_temp(&tsens_dev, &temp);

	if (!ENABLED) {
		return;
	}

	if (info.throttling)
	{
		if (temp < (TEMP_THRESHOLD - info.safe_diff))
		{
			now = ktime_to_us(ktime_get());

			if (now < (info.limit_cpu_time + POLL_INTERVAL))
				goto reschedule;

			limit_cpu_freqs(info.cpuinfo_max_freq);
			info.throttling = false;
			goto reschedule;
		}
	}

	if (temp >= TEMP_THRESHOLD + LEVEL_HELL)
		freq = FREQ_HELL;
	else if (temp >= TEMP_THRESHOLD + LEVEL_VERY_HOT)
		freq = FREQ_VERY_HOT;
	else if (temp >= TEMP_THRESHOLD + LEVEL_HOT)
		freq = FREQ_HOT;
	else if (temp > TEMP_THRESHOLD)
		freq = FREQ_WARM;

	if (freq)
	{
		limit_cpu_freqs(freq);

		if (!info.throttling)
			info.throttling = true;
	}

reschedule:
	schedule_delayed_work_on(0, &check_temp_work, msecs_to_jiffies(250));
}

static int msm_thermal_dev_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device_node *node = pdev->dev.of_node;
	struct msm_thermal_data data;

	memset(&data, 0, sizeof(struct msm_thermal_data));

	ret = of_property_read_u32(node, "qcom,sensor-id", &data.sensor_id);
	if (ret)
		return ret;

	WARN_ON(data.sensor_id >= TSENS_MAX_SENSORS);

        memcpy(&msm_thermal_info, &data, sizeof(struct msm_thermal_data));

        INIT_DELAYED_WORK(&check_temp_work, check_temp);
        schedule_delayed_work_on(0, &check_temp_work, 10 * HZ);

	cpufreq_register_notifier(&msm_thermal_cpufreq_notifier,
			CPUFREQ_POLICY_NOTIFIER);

	return ret;
}

static int msm_thermal_dev_remove(struct platform_device *pdev)
{
	cpufreq_unregister_notifier(&msm_thermal_cpufreq_notifier,
                        CPUFREQ_POLICY_NOTIFIER);
	return 0;
}

static struct of_device_id msm_thermal_match_table[] = {
	{.compatible = "qcom,msm-thermal"},
	{},
};

static struct platform_driver msm_thermal_device_driver = {
	.probe = msm_thermal_dev_probe,
	.remove = msm_thermal_dev_remove,
	.driver = {
		.name = "msm-thermal",
		.owner = THIS_MODULE,
		.of_match_table = msm_thermal_match_table,
	},
};

int __init  msm_thermal_device_init(void)
{
	return platform_driver_register(&msm_thermal_device_driver);
}

void __exit msm_thermal_device_exit(void)
{
	platform_driver_unregister(&msm_thermal_device_driver);
}

late_initcall(msm_thermal_device_init);
module_exit(msm_thermal_device_exit);

