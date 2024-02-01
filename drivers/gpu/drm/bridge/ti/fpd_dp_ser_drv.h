/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef __FPD_DP_SER_DEV_h__
#define __FPD_DP_SER_DEV_h__

#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <drm/drm_modes.h>

#define FPD_DP_SER_TX_ADD                  0x14
#define FPD_DP_SER_RX_ADD_A                0x30
#define FPD_DP_SER_MCU_ADD                 0x78

#define FPD_DP_ARRAY_SIZE                  4

#define DS90UB983                          0
#define DS90UB984                          1
#define DS90UBMCU                          2

#define NUM_DEVICE                         3
#define ADAPTER_PP_DEV_NAME                "0000:00:15.1"

enum fdp_dp_ser_strap_rate {
	FPD4_Strap_Rate_0,
	FPD4_Strap_Rate_3_375,
	FPD4_Strap_Rate_6_75,
	FPD4_Strap_Rate_10_8,
	FPD4_Strap_Rate_13_5,
};

struct fpd_dp_ser_priv {
	struct device *dev;
	struct i2c_adapter *i2c_adap;
	u8 FPDConf;
	u8 FPD4_Strap_Rate_P0;
	u8 FPD4_Strap_Rate_P1;
	struct i2c_client *priv_dp_client[NUM_DEVICE];
	struct delayed_work delay_work;
	struct workqueue_struct *wq;
	int count;
};

void fpd_dp_ser_module_exit(void);
int fpd_dp_ser_module_init(void);

int fpd_dp_ser_init(void);

#endif /* __FPD_DP_SER_DRV__ */
