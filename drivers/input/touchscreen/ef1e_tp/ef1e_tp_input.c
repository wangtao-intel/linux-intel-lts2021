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

#include <linux/input-event-codes.h>
#include <linux/input/mt.h>

#include "ef1e_tp.h"
#include "ef1e_tp_input.h"
#include "ef1e_tp_protocol.h"

int tp_input_dev_init(struct tp_priv *priv)
{
	int ret = 0;

	priv->input_dev = input_allocate_device();
	if (priv->input_dev == NULL) {
		pr_err("%s: Failed to allocate input device\n", __func__);
		return -ENOMEM;
	}
	priv->input_dev->name = TP_INPUT_DEV_NAME;

	input_set_capability(priv->input_dev, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(priv->input_dev, EV_ABS, ABS_MT_POSITION_Y);

	input_set_abs_params(priv->input_dev, ABS_MT_POSITION_X, 0, TP_WIDTH - 1, 0, 0);
	input_set_abs_params(priv->input_dev, ABS_MT_POSITION_Y, 0, TP_HEIGHT - 1, 0, 0);
	input_set_abs_params(priv->input_dev, ABS_MT_WIDTH_MAJOR, 0, 200, 0, 0);
	input_set_abs_params(priv->input_dev, ABS_MT_TOUCH_MAJOR, 0, 200, 0, 0);
	input_set_abs_params(priv->input_dev, ABS_MT_PRESSURE, 0, TP_PRESSURE_MAX, 0, 0);

	__set_bit(BTN_TOUCH, priv->input_dev->keybit);

	ret = input_mt_init_slots(priv->input_dev, TP_MAX_POINTS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret) {
		pr_err("%s: Failed to initialize MT slots: %d\n", __func__, ret);
		return ret;
	}
	ret = input_register_device(priv->input_dev);
	if (ret) {
		pr_err("%s: Failed to register input device\n", __func__);
		return ret;
	}
	return 0;
}


void tp_input_dev_destroy(struct tp_priv *priv)
{
	input_unregister_device(priv->input_dev);
}


int tp_input_dev_report(struct tp_priv *priv, struct tp_report_data *data)
{
	int i;
	struct tp_point_data *p;
	unsigned int x, y, w, pressure;

	for (i = 0; i < data->number_points; ++i) {
		p = &data->point_data[i];
		x = (((unsigned int) p->x_h) << 8) | p->x_l;
		y = (((unsigned int) p->y_h) << 8) | p->y_l;
		pressure = TP_PRESSURE_MAX;
		w = 5;
		if (p->status == TP_PROTOCOL_TP_STATUS_RELEASE) {
			pressure = 0;
			w = 0;
		}
		input_mt_slot(priv->input_dev, p->id);
		input_mt_report_slot_state(priv->input_dev, MT_TOOL_FINGER, pressure > 0);
		input_report_abs(priv->input_dev, ABS_MT_POSITION_X, x);
		input_report_abs(priv->input_dev, ABS_MT_POSITION_Y, y);
		input_report_abs(priv->input_dev, ABS_MT_TOUCH_MAJOR, w);
		input_report_abs(priv->input_dev, ABS_MT_WIDTH_MAJOR, w);
		input_report_abs(priv->input_dev, ABS_MT_PRESSURE, pressure);
		input_report_key(priv->input_dev, BTN_TOUCH, pressure);
	}
	input_mt_sync_frame(priv->input_dev);
	input_sync(priv->input_dev);
	return 0;
}

