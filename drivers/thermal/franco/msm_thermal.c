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
 * 9. Updated Shoaib's Core Control to v2.0 with Improvements and BUG-Fixes as well as Support for Hexa-Core big.LITTLE SoCs.
 * 10. Altered the Formatting of the Codes (looks cleaner and more beautiful now).
 * 11. Updated Shoaib's Core Control to v2.1 (AiO HotPlug's Dependency Removed).
 * 12. Updated Shoaib's Core Control to v2.2 (All Checks Removed for Thermal Table). 
 * 13. Updated Shoaib's Core Control to v2.4 (Core 0 Permission Toggle replaced with a Native one).
 * 14. Disable Frequency/Thermal Functionality (only 1 Throttle Point is available now). 
 * 15. Remove Temperature Step Functionality.
 * 16. Updated Shoaib's Core Control v2.6 (Minor Improvements).
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

#include <linux/of.h>
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/msm_tsens.h>
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>

// Temp Threshold is the LOWEST Level to Start Throttling.
#define _temp_threshold		60

int TEMP_THRESHOLD 		= _temp_threshold;

#if (NR_CPUS == 4 || NR_CPUS == 6 || NR_CPUS == 8)
      #ifdef CONFIG_ARCH_MSM8916
             int FREQ_WARM 	= 800000;
      #else
	   int FREQ_WARM 	= 864000;
      #endif
#endif

#ifdef CONFIG_CORE_CONTROL
// Essentials for Shoaib's Core Control.
#if (NR_CPUS == 4)
    bool core_control = false;
#elif (NR_CPUS == 6 || NR_CPUS == 8)    
      bool core_control = true;
#endif
static struct kobject *cc_kobj;
#endif

#if (NR_CPUS == 6 || NR_CPUS == 8)	// Assume Hexa/Octa-Core SoCs to be based on big.LITTLE architecture.
// Permission to Disable Core 0 Toggle.
static int Core0_Toggle = 0;
module_param (Core0_Toggle, int, 0644);
#endif

/* Temperature Threshold Storage */
static int set_temp_threshold(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	int i;

	ret = kstrtouint(val, 10, &i);
	if (ret || i < 40 || i > 90)
	   return -EINVAL;

	ret = param_set_int(val, kp);

	return ret;
}

static struct kernel_param_ops temp_threshold_ops = {
	.set = set_temp_threshold,
	.get = param_get_int,
};

module_param_cb(temp_threshold, &temp_threshold_ops, &TEMP_THRESHOLD, 0644);

/* Frequency Limit Storage */
static int set_freq_limit(const char *val, const struct kernel_param *kp)
{
	int ret = 0;
	int i;
	struct cpufreq_policy *policy;
	static struct cpufreq_frequency_table *tbl = NULL;
	
	ret = kstrtouint(val, 10, &i);

	// Disable Frequency/Thermal Table Functionality.
	return -EINVAL;

        policy = cpufreq_cpu_get(0);
	tbl = cpufreq_frequency_get_table(0);

	ret = param_set_int(val, kp);

	return ret;
}

static struct kernel_param_ops freq_limit_ops = {
	.set = set_freq_limit,
	.get = param_get_int,
};

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

	#ifdef CONFIG_CORE_CONTROL
	// Begin HotPlug Mechanism for Shoaib's Core Control
	if (core_control)
	{
	   // Assume Quad-Core SoCs to be of Traditional Configuration.
	   #if (NR_CPUS == 4)
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
	   // Assume Hexa-Core SoCs to be of big.LITTLE Configuration.
	   #elif (NR_CPUS == 6)
	         if (temp > 80)
	         {
		    if (!cpu_online(2))
	               cpu_up(2);
	            if (!cpu_online(3))
	               cpu_up(3);

	            if (cpu_online(1))
	               cpu_down(1);
	            // Disable Core 0 only if Permission is Granted.
	            if (Core0_Toggle == 1)
		    {
	               if (cpu_online(0))
	                  cpu_down(0);
		    }
		    else
		    {
			if (!cpu_online(0))
			   cpu_up(0);
		    }
		    if (cpu_online(5))
	               cpu_down(5);
	            if (cpu_online(4))
	               cpu_down(4);
	         }
		 else if (temp > 55 && temp <= 65)
		 {
		      if (!cpu_online(2))
	                 cpu_up(2);
	              if (!cpu_online(3))
	                 cpu_up(3);
		      if (!cpu_online(4))
	                 cpu_up(4);
	              if (!cpu_online(5))
	                 cpu_up(5);
		      
		      if (cpu_online(1))
	                 cpu_down(1);
	              // Disable Core 0 only if Permission is Granted.
	              if (Core0_Toggle == 1)
		      {
	                 if (cpu_online(0))
	                    cpu_down(0);
		      }
		      else
		      {
			  if (!cpu_online(0))
			     cpu_up(0);
		      }
	          }
		  else if (temp == 50)
		  {
		          int cpu;
	                  for_each_possible_cpu(cpu)
	                      if (!cpu_online(cpu))
		                 cpu_up(cpu);
	          }
	   // Assume Octa-Core SoCs to be of big.LITTLE Configuration.
	   #elif (NR_CPUS == 8)
	         if (temp > 80)
	         {
	            if (cpu_online(3))
	      	       cpu_down(3);
	            if (cpu_online(2))
	               cpu_down(2);
	            if (cpu_online(1))
	               cpu_down(1);
		    // Disable Core 0 only if Permission is Granted.
	            if (Core0_Toggle == 1)
		    {
	               if (cpu_online(0))
	                  cpu_down(0);
		    }
		    else
		    {
			if (!cpu_online(0))
			   cpu_up(0);
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
		         // Disable Core 0 only if Permission is Granted.
		         if (Core0_Toggle == 1)
		         {
	                    if (cpu_online(0))
	                       cpu_down(0);
		         }
		         else
		    	 {
			     if (!cpu_online(0))
			        cpu_up(0);
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
           #endif   
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

	if (temp > TEMP_THRESHOLD)
	   freq = FREQ_WARM;

	if (freq) 
        {
	   limit_cpu_freqs(freq);

	   if (!info.throttling)
	      info.throttling = true;
	}

reschedule:
	#ifdef CONFIG_WQ_POWER_EFFICIENT_DEFAULT
	queue_delayed_work(system_power_efficient_wq, &check_temp_work, msecs_to_jiffies(1000));
	#else
	schedule_delayed_work_on(0, &check_temp_work, msecs_to_jiffies(1000));
	#endif
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
	if (ret)
	{
	   pr_err("Invalid input %s. err:%d\n", buf, ret);
	   goto done_store_cc;
	}
	

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
	// Initialize Shoaib's Core Control Driver only for Quad-Core or Higher SoCs.
	if (num_possible_cpus() >= 4)
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
