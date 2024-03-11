/*
 * Copyright 2024 Intel Corp.
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
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef EF1E_TP_H
#define EF1E_TP_H

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input/touchscreen.h>

#include "ef1e_tp_protocol.h"

#define TP_WIDTH  2880
#define TP_HEIGHT 1620

struct tp_priv {
	struct i2c_adapter *i2c_adap;
	struct i2c_client *i2c_ser_client;
	struct i2c_client *i2c_des_client;
	struct i2c_client *i2c_mcu_client;
	u8 i2c_ser_address;
	u8 i2c_des_address;
	u8 i2c_mcu_address;
	struct gpio_desc *tp_gpio;
	int tp_irq;

	bool polling;
	struct task_struct *polling_kthread;

	struct input_dev *input_dev;
};

#endif /* EF1E_TP_H */

