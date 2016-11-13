/*
 * Author: Paul Reioux aka Faux123 <reioux@gmail.com>
 *
 * WCD93xx sound control module
 * Copyright 2013~2014 Paul Reioux
 *
 * Adapted to Qualcomm msm8x16-wcd codec
 * Mr. AW (Aditya Wicaksono) <mr.aw173@gmail.com>
 *
 * Credits to robcore for fixing the 'Wrong Values at Boot' issue of the Driver.
 *
 * Updated to v4.1 by Shoaib Anwar a.k.a Shoaib0597 <shoaib0595@gmail.com> :
 * 1. Fixed the Registry and Cache Size according to the Gains (MSM8916 supports fewer Gains).
 * 2. Fixed the Mechanism of Gains to make this port perfect (thus, Negative Values work now and there is no Low-Volume while in-Call issue).
 * 3. Added an Enable/Disable Toggle with a Smart Workaround (this allows full compatibility with Kernel Adiutor-MOD).
 * 4. Removed Mic-Gain and Camcorder Mic-Gain (both of these Gains don't work the way they're supposed to on Tomato(+) Device).
 * 5. Altered the Formatting of the Codes (looks cleaner and more beautiful now).
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/kallsyms.h>
#include "msm8x16_wcd_registers.h"
#include <linux/mfd/wcd9xxx/core.h>

#define SOUND_CONTROL_MAJOR_VERSION	4
#define SOUND_CONTROL_MINOR_VERSION	1

extern struct snd_soc_codec *fauxsound_codec_ptr;

extern bool enable_fs;

unsigned int snd_ctrl_locked = 2;

unsigned int msm8x16_wcd_read(struct snd_soc_codec *codec, unsigned int reg);
int msm8x16_wcd_write(struct snd_soc_codec *codec, unsigned int reg, unsigned int value);

#define REG_SZ	3
static unsigned int cached_regs[] = {0, 0, 0};

static unsigned int *cache_select(unsigned int reg)
{
	unsigned int *out = NULL;

        switch (reg) 
	    {
               case MSM8X16_WCD_A_CDC_RX1_VOL_CTL_B2_CTL:
		            out = &cached_regs[0];
			   break;
               case MSM8X16_WCD_A_CDC_RX2_VOL_CTL_B2_CTL:
		    	    out = &cached_regs[1];
			   break;
               case MSM8X16_WCD_A_CDC_RX3_VOL_CTL_B2_CTL:
		    		out = &cached_regs[2];
			   break;
        }
    return out;
}

void snd_hax_cache_write(unsigned int reg, unsigned int value)
{
	unsigned int *tmp = cache_select(reg);

	if (tmp != NULL)
	   *tmp = value;
}
EXPORT_SYMBOL(snd_hax_cache_write);

unsigned int snd_hax_cache_read(unsigned int reg)
{
	if (cache_select(reg) != NULL)
	   return *cache_select(reg);
	else
	   return -1;
}
EXPORT_SYMBOL(snd_hax_cache_read);

int snd_hax_reg_access(unsigned int reg)
{
	int ret = 1;

	switch (reg) 
	{      
		   /* Digital Headphones Gain */
	       case MSM8X16_WCD_A_CDC_RX1_VOL_CTL_B2_CTL:
	       case MSM8X16_WCD_A_CDC_RX2_VOL_CTL_B2_CTL:
		   /* Speaker Gain */
	       case MSM8X16_WCD_A_CDC_RX3_VOL_CTL_B2_CTL:
		   if ((enable_fs > 0) && (snd_ctrl_locked > 0))
		      ret = 0;
		   break;

	       default:
		      break;
	}
	return ret;
}
EXPORT_SYMBOL(snd_hax_reg_access);

static unsigned int selected_reg = 0xdeadbeef;

static ssize_t sound_reg_select_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	if (!enable_fs)
	   return count;

	sscanf(buf, "%d", &selected_reg);

	return count;
}

static ssize_t sound_reg_read_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	if (selected_reg == 0xdeadbeef)
	   return -1;
	else
	   return sprintf(buf, "%d\n", msm8x16_wcd_read(fauxsound_codec_ptr, selected_reg));
}

static ssize_t sound_reg_write_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int out;

	sscanf(buf, "%d", &out);

	if (!enable_fs)
	   return count;

	if (selected_reg != 0xdeadbeef)
	   msm8x16_wcd_write(fauxsound_codec_ptr, selected_reg, out);

	return count;
}

static ssize_t speaker_gain_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d %d\n", msm8x16_wcd_read(fauxsound_codec_ptr, MSM8X16_WCD_A_CDC_RX3_VOL_CTL_B2_CTL), msm8x16_wcd_read(fauxsound_codec_ptr, MSM8X16_WCD_A_CDC_RX3_VOL_CTL_B2_CTL));
}

static ssize_t speaker_gain_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int lval, rval;

	sscanf(buf, "%d %d", &lval, &rval);

	if (!enable_fs)
	   return count;

	snd_ctrl_locked = 0;
	msm8x16_wcd_write(fauxsound_codec_ptr, MSM8X16_WCD_A_CDC_RX3_VOL_CTL_B2_CTL, lval);
        msm8x16_wcd_write(fauxsound_codec_ptr, MSM8X16_WCD_A_CDC_RX3_VOL_CTL_B2_CTL, rval);
	snd_ctrl_locked = 2;

	return count;
}

static ssize_t headphone_gain_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d %d\n", msm8x16_wcd_read(fauxsound_codec_ptr, MSM8X16_WCD_A_CDC_RX1_VOL_CTL_B2_CTL), msm8x16_wcd_read(fauxsound_codec_ptr, MSM8X16_WCD_A_CDC_RX2_VOL_CTL_B2_CTL));
}

static ssize_t headphone_gain_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int lval, rval;

	sscanf(buf, "%d %d", &lval, &rval);

	if (!enable_fs)
	   return count;

	snd_ctrl_locked = 0;
	msm8x16_wcd_write(fauxsound_codec_ptr, MSM8X16_WCD_A_CDC_RX1_VOL_CTL_B2_CTL, lval);
	msm8x16_wcd_write(fauxsound_codec_ptr, MSM8X16_WCD_A_CDC_RX2_VOL_CTL_B2_CTL, rval);
	snd_ctrl_locked = 2;

	return count;
}

static ssize_t sound_control_version_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "Version: %d.%d\n", SOUND_CONTROL_MAJOR_VERSION, SOUND_CONTROL_MINOR_VERSION);
}

static struct kobj_attribute sound_reg_sel_attribute =
	__ATTR(sound_reg_sel,
		0222,
		NULL,
		sound_reg_select_store);

static struct kobj_attribute sound_reg_read_attribute =
	__ATTR(sound_reg_read,
		0444,
		sound_reg_read_show,
		NULL);

static struct kobj_attribute sound_reg_write_attribute =
	__ATTR(sound_reg_write,
		0666,
		NULL,
		sound_reg_write_store);

static struct kobj_attribute headphone_gain_attribute =
	__ATTR(gpl_headphone_gain,
		0666,
		headphone_gain_show,
		headphone_gain_store);

static struct kobj_attribute speaker_gain_attribute =
	__ATTR(gpl_speaker_gain,
		0666,
		speaker_gain_show,
		speaker_gain_store);

static struct kobj_attribute sound_control_version_attribute =
	__ATTR(gpl_sound_control_version,
		0444,
		sound_control_version_show, NULL);

static struct attribute *sound_control_attrs[] =
{
	&sound_reg_sel_attribute.attr,
	&sound_reg_read_attribute.attr,
	&sound_reg_write_attribute.attr,
	&headphone_gain_attribute.attr,
	&speaker_gain_attribute.attr,
	&sound_control_version_attribute.attr,
	NULL,
};

static struct attribute_group sound_control_attr_group =
{
	.attrs = sound_control_attrs,
};

static struct kobject *sound_control_kobj;

static int sound_control_init(void)
{
	int sysfs_result;

	enable_fs = 1;
	snd_ctrl_locked = 2;

	sound_control_kobj = kobject_create_and_add("sound_control_3", kernel_kobj);

	if (!sound_control_kobj) 
	{
	   pr_err("%s sound_control_kobj create failed!\n", __FUNCTION__);
	   return -ENOMEM;
	}

	sysfs_result = sysfs_create_group(sound_control_kobj, &sound_control_attr_group);

	if (sysfs_result) 
	{
	   pr_info("%s sysfs create failed!\n", __FUNCTION__);
	   kobject_put(sound_control_kobj);
	}

	return sysfs_result;
}

static void sound_control_exit(void)
{
	if (sound_control_kobj != NULL)
	   kobject_put(sound_control_kobj);
}
module_init(sound_control_init);
module_exit(sound_control_exit);

MODULE_LICENSE("GPLv2");
MODULE_AUTHOR("Paul Reioux <reioux@gmail.com>");
MODULE_DESCRIPTION("WCD93xx Sound Engine v4.x");
