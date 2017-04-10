/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 * Copyright (c) 2015 Francisco Franco
 *
 * Heavily Enhanced and Modified by Yoinx.
 *
 * Modified by Shoaib Anwar a.k.a Shoaib0597 <shoaib0595@gmail.com>:
 * 1. Switched to an easy implementation of Polling Interval, setting it at a constant of 1 second.
 * 2. Fixed a BUG which prevented the users from applying certain Frequencies to the user-defined Temperature-Limits.
 * 3. Changed the Default Values to more Efficient Parameters for Better Heat-Management and Battery-Life.
 * 4. Switched to Power Efficient WorkQueues for lesser footprint on CPU.
 * 5. Removed Two Frequency Throttle Points, only two are now available as against four (four are unnecessary as well as having only two also reduces calculations making the Driver lighter).
 * 6. Added a function to allow users to configure whether Core 0 should be disabled or not.
 * 7. Added a check to make sure that Frequency Input from the user is only taken when Permission to Disable Core has not been granted even once (for big.LITTLE SoC) to prevent freezes.
 * 8. Introduced Shoaib's Core Control, an Automatic HotPlug based on Temperature.
 * 9. Altered the Formatting of the Codes (looks cleaner and more beautiful now).
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
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>
#include <linux/of.h>

// Temp Threshold is the LOWEST Level to Start Throttling.
#define _temp_threshold		55

// Temperature Step (manages the GAPs between Different Thermal Throttle Points).
#define _temp_step		5

int TEMP_THRESHOLD 	= _temp_threshold;
int TEMP_STEP 		= _temp_step;
int LEVEL_HOT 		= _temp_threshold + _temp_step;
int FREQ_HOT 		= 800000;
#if (NR_CPUS == 8)
int FREQ_WARM 		= 1113600;
#else
int FREQ_WARM 		= 1094400;
#endif

#ifdef CONFIG_CORE_CONTROL
// Essentials for Shoaib's Core Control.
bool core_control = true;
static struct kobject *cc_kobj;
#endif

#ifdef CONFIG_AiO_HotPlug
extern int AiO_HotPlug;
#endif

#if (NR_CPUS == 8)	// Assume Octa-Core SoCs to be based on big.LITTLE architecture.
// Permission to Disable Core 0 Toggle.
extern bool hotplug_boost;
#endif

// Variables to know the user-behaviour.
int flag = 0;
int count = 0;

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
	
	LEVEL_HOT      = i + TEMP_STEP;
	
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
	// Restrict the Values to 1-6 for the Purpose of Safety.
	if (i < 1 || i > 6)
	   return -EINVAL;
	
	LEVEL_HOT = TEMP_THRESHOLD + i;
	
	ret = param_set_int(val, kp);

	return ret;
}

static struct kernel_param_ops temp_step_ops = {
	.set = set_temp_step,
	.get = param_get_int,
};

module_param_cb(temp_step, &temp_step_ops, &TEMP_STEP, 0644);

/* Frequency Limit Storage */
static int set_freq_limit(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	int i;
	struct cpufreq_policy *policy;
	static struct cpufreq_frequency_table *tbl = NULL;
	
	ret = kstrtouint(val, 10, &i);

	// Don't store any values if permission to disable Core 0 has been granted once on a big.LITTLE SoC to avoid the situation where the whole system freezes. To again use this function, the phone must be rebooted.
	if (ret || count == 1)
	   return -EINVAL;

        policy = cpufreq_cpu_get(0);
	tbl = cpufreq_frequency_get_table(0);

	ret = param_set_int(val, kp);

	// Set flag to 1 if Thermal Frequency Table have been changed by user.
	flag = 1;

	return ret;
}

static struct kernel_param_ops freq_limit_ops = {
	.set = set_freq_limit,
	.get = param_get_int,
};

module_param_cb(freq_hot, &freq_limit_ops, &FREQ_HOT, 0644);
module_param_cb(freq_warm, &freq_limit_ops, &FREQ_WARM, 0644);

static struct thermal_info {
	uint32_t cpuinfo_max_freq;
	uint32_t limited_max_freq;
	unsigned int safe_diff;
	bool throttling;
	bool pending_change;
	u64 limit_cpu_time;
} info = {
	.cpuinfo_max_freq = UINT_MAX,
	.limited_max_freq = UINT_MAX,
	.safe_diff = 5,
	.throttling = false,
	.pending_change = false,
};

struct msm_thermal_data msm_thermal_info;

static struct delayed_work check_temp_work;

static int msm_thermal_cpufreq_callback(struct notifier_block *nfb, unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;

	if (event != CPUFREQ_ADJUST && !info.pending_change)
	   return 0;

	cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq, info.limited_max_freq);

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

	get_online_cpus();
	for_each_online_cpu(cpu) 
        {
	    cpufreq_update_policy(cpu);
	    pr_info("%s: Setting cpu%d max frequency to %d\n", KBUILD_MODNAME, cpu, info.limited_max_freq);
	}
	put_online_cpus();

	info.pending_change = false;
}

static void __ref check_temp(struct work_struct *work)
{
	struct tsens_device tsens_dev;
	uint32_t freq = 0;
	long temp = 0;

	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	tsens_get_temp(&tsens_dev, &temp);

	// Switch on Core 0 again as soon as Permission to disable it is denied.
	if (!cpu_online(0))
	{
	   count = 1;
	   if (hotplug_boost == false)
	      cpu_up(0);
	}

	#ifdef CONFIG_CORE_CONTROL
	// Begin HotPlug Mechanism for Shoaib's Core Control
	if (core_control)
	{
	   // If SoC is an Octa-Core one, assume it to be based on big.LITTLE.
	   if (NR_CPUS == 8)
	   {
	      if (temp > 80)
	      {
	         if (cpu_online(3))
	      	    cpu_down(3);
	         if (cpu_online(2))
	            cpu_down(2);
	         if (cpu_online(1))
	            cpu_down(1);
		 // Disable Core 0 only if Permission is granted and Thermal Frequency Table has not been changed by user.
	         if (hotplug_boost == true && flag == 0)
		 {
	            if (cpu_online(0))
	               cpu_down(0);
		 }
	         if (cpu_online(7))
	            cpu_down(7);
	         if (cpu_online(6))
	            cpu_down(6); 
	      }
	      else if (temp > 55 && temp <= 65)
	      {
		      if (!cpu_online(6))
		         cpu_up(6);
	              if (!cpu_online(7))
	 	         cpu_up(7);

	              if (cpu_online(3))
	      	         cpu_down(3);
	              if (cpu_online(2))
	                 cpu_down(2);
		      if (cpu_online(1))
	                 cpu_down(1);
		      // Disable Core 0 only if Permission is granted and Thermal Frequency Table has not been changed by user.
		      if (hotplug_boost == true && flag == 0)
		      {
	              if (cpu_online(0))
	                 cpu_down(0);
		      }
		      
	      }
	      else if (temp > 45 && temp <= 50)
	      {
		      if (!cpu_online(0))
	                 cpu_up(0);
	              if (!cpu_online(1))
	                 cpu_up(1);
	          
	              if (cpu_online(3))
	                 cpu_down(3);
	              if (cpu_online(2))
	                 cpu_down(2);
	      }
	      else if (temp == 40)
	      {
	              int cpu;
	              for_each_possible_cpu(cpu)
	                  if (!cpu_online(cpu))
		             cpu_up(cpu);
	      }
	   }
	   // If SoC is a Quad-Core one, assume it to be of Traditional Configuration.
	   else if (NR_CPUS == 4)
	   {
	           if (temp > 80)
	           {
	           if (cpu_online(3))
	      	      cpu_down(3);
	           if (cpu_online(2))
	              cpu_down(2);
	           if (cpu_online(1))
	              cpu_down(1); 
	           }
	           else if (temp > 70 && temp <= 75)
	           {
			   if (!cpu_online(1))
	                      cpu_up(1);

	                   if (cpu_online(3))
	      	              cpu_down(3);
	                   if (cpu_online(2))
	                      cpu_down(2);
	           }
	           else if (temp > 60 && temp <= 65)
	           {
	                   if (!cpu_online(2))
	                      cpu_up(2);

	                   if (cpu_online(3))
	                      cpu_down(3);
	           }
	           else if (temp == 55)
	           {
	                   int cpu;
	                   for_each_possible_cpu(cpu)
	                       if (!cpu_online(cpu))
		                  cpu_up(cpu);
	           }
	   }
        }
	// End HotPlug Mechanism for Shoaib's Core Control
	#endif

	if (info.throttling) 
        {
	   if (temp < (TEMP_THRESHOLD - info.safe_diff)) 
           {
	      limit_cpu_freqs(info.cpuinfo_max_freq);
	      info.throttling = false;
	      goto reschedule;
	   }
	}

	if (temp >= LEVEL_HOT)
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
	queue_delayed_work(system_power_efficient_wq, &check_temp_work, msecs_to_jiffies(1000));
}

#ifdef CONFIG_CORE_CONTROL
// Begin sysFS for Shoaib's Core Control
static ssize_t show_cc_enabled(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", core_control);
}

static ssize_t __ref store_cc_enabled(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int val = 0;

	ret = kstrtoint(buf, 10, &val);
	#ifdef CONFIG_AiO_HotPlug
	// Allow Shoaib's Core Control to be Enabled only if AiO HotPlug is Disabled.
	if (ret || AiO_HotPlug == 1) 
	{
	   pr_err("Invalid input %s. err:%d\n", buf, ret);
	   goto done_store_cc;
	}
	#else
	if (ret)
	{
	   pr_err("Invalid input %s. err:%d\n", buf, ret);
	   goto done_store_cc;
	}
	#endif
	

	if (core_control == !!val)
	   goto done_store_cc;

	core_control = !!val;

	if (!core_control)
	{
	   int cpu;
	   /* Wake-Up All the Sibling Cores */
	   for_each_possible_cpu(cpu)
	       if (!cpu_online(cpu))
		  cpu_up(cpu);
	}

done_store_cc:
	return count;
}

static __refdata struct kobj_attribute cc_enabled_attr = 
__ATTR(core_control, 0644, show_cc_enabled, store_cc_enabled);

static __refdata struct attribute *cc_attrs[] = {
	&cc_enabled_attr.attr,
	NULL,
};

static __refdata struct attribute_group cc_attr_group = {
	.attrs = cc_attrs,
};

static __init int msm_thermal_add_cc_nodes(void)
{
	struct kobject *module_kobj = NULL;
	int ret = 0;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) 
	{
	   pr_err("cannot find kobject\n");
	   ret = -ENOENT;
	   goto done_cc_nodes;
	}

	cc_kobj = kobject_create_and_add("core_control", module_kobj);
	if (!cc_kobj) 
	{
	   pr_err("cannot create core control kobj\n");
	   ret = -ENOMEM;
	   goto done_cc_nodes;
	}

	ret = sysfs_create_group(cc_kobj, &cc_attr_group);
	if (ret) 
	{
	   pr_err("cannot create sysfs group. err:%d\n", ret);
	   goto done_cc_nodes;
	}

	return 0;

done_cc_nodes:
	if (cc_kobj)
	   kobject_del(cc_kobj);
	return ret;
}
// End sysFS for Shoaib's Core Control
#endif

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
        schedule_delayed_work(&check_temp_work, 5);

	cpufreq_register_notifier(&msm_thermal_cpufreq_notifier, CPUFREQ_POLICY_NOTIFIER);

	return ret;
}

static int msm_thermal_dev_remove(struct platform_device *pdev)
{
	cpufreq_unregister_notifier(&msm_thermal_cpufreq_notifier, CPUFREQ_POLICY_NOTIFIER);
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

static int __init msm_thermal_device_init(void)
{
	#ifdef CONFIG_CORE_CONTROL
	// Initialize Shoaib's Core Control Driver 
	if (num_possible_cpus() > 1)
	   msm_thermal_add_cc_nodes();
	#endif

	return platform_driver_register(&msm_thermal_device_driver);
}

static void __exit msm_thermal_device_exit(void)
{
	platform_driver_unregister(&msm_thermal_device_driver);
}

late_initcall(msm_thermal_device_init);
module_exit(msm_thermal_device_exit);
