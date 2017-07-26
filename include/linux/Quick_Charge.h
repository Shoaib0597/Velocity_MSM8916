/* Header File for Quick Charge v1.0 Driver.
 *
 * Copyright (c) 2017, Shoaib Anwar <Shoaib0595@gmail.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef __LINUX_QUICK_CHARGE_H
#define __LINUX_QUICK_CHARGE_H

extern int QC_Toggle;
extern int Dynamic_Current;
extern int Charging_Profile;

extern void batt_level (int);
extern void charging (int);
extern void actual_current (int);
extern void ic_vendor (int);
		  
#endif
