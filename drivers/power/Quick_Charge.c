/* Quick Charge v1.0, a Fast-Charge Driver Based on the Idea of Qualcomm's 
 * Quick-Charge Technology.
 *
 * For this Driver to Work properly, a Wall-Charger Capable of Supplying 
 * 1.5A Output is Required.
 *
 * Copyright (c) 2017, Shoaib Anwar <Shoaib0595@gmail.com>.
 *
 * Based on ThunderCharge Current Control, by Varun Chitre <varun.chitre15@gmail.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/Quick_Charge.h>

#define ENABLED			1
#define QUICK_CHARGE		"Quick_Charge"
#define CURRENT			1250

int QC_Toggle = ENABLED;
int Dynamic_Current = CURRENT;

void batt_level (int Battery_Status)
{
	if (Battery_Status >= 0 && Battery_Status <= 60)
	   Dynamic_Current = 1500;
	else if (Battery_Status >= 61 && Battery_Status <= 90)
		Dynamic_Current = 1250;
	else if (Battery_Status >= 91 && Battery_Status <= 100)
  	        Dynamic_Current = 1000;
}

static ssize_t qc_toggle_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d", QC_Toggle);
}

static ssize_t qc_toggle_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val;
	sscanf (buf, "%d", &val);
	switch (val)
	{
	case 0:
	case 1:
		QC_Toggle = val;
	break;
	default:
		pr_info("%s: Invalid Value", QUICK_CHARGE);
    	break;
	}
return count;
}

static ssize_t dynamic_current_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d", Dynamic_Current);
}

static struct kobj_attribute qc_toggle_attribute =
	__ATTR(QC_Toggle,
		0666,
		qc_toggle_show,
		qc_toggle_store);

static struct kobj_attribute dynamic_current_attribute =
	__ATTR(Dynamic_Current,
		0444,
		dynamic_current_show, 
		NULL);

static struct attribute *charger_control_attrs[] =
	{
		&qc_toggle_attribute.attr,
		&dynamic_current_attribute.attr,
		NULL,
	};
	
static struct attribute_group chgr_control_attr_group =
	{
		.attrs = charger_control_attrs,
	};

static struct kobject *charger_control_kobj;

static int charger_control_probe(void)
{
	int sysfs_result;
	printk(KERN_DEBUG "[%s]\n",__func__);

	charger_control_kobj = kobject_create_and_add("Quick_Charge", kernel_kobj);

	if (!charger_control_kobj) 
	{
	   pr_err("%s Interface create failed!\n", __FUNCTION__);
	   return -ENOMEM;
        }

	sysfs_result = sysfs_create_group(charger_control_kobj, &chgr_control_attr_group);

	if (sysfs_result) 
	{
	   pr_info("%s sysfs create failed!\n", __FUNCTION__);
	   kobject_put(charger_control_kobj);
	}
	return sysfs_result;
}

static void charger_control_remove(void)
{
	if (charger_control_kobj != NULL)
	   kobject_put(charger_control_kobj);
}

module_init(charger_control_probe);
module_exit(charger_control_remove);
MODULE_LICENSE("GPL and Additional Rights");
MODULE_AUTHOR("Shoaib Anwar <Shoaib0595@gmail.com>");
MODULE_DESCRIPTION("Quick Charge v1.0");
