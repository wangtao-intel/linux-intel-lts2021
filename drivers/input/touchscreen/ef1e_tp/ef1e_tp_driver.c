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

#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/module.h>

#include "ef1e_tp.h"
#include "ef1e_tp_input.h"
#include "ef1e_tp_protocol.h"

#define ADAPTER_PP_DEV_NAME			"0000:00:15.1"

#define I2C_SER_ADDRESS				0x14
#define I2C_DES_ADDRESS				0x30
#define I2C_MCU_ADDRESS				0x78


/* Set polling to non-zero to enable polling mode for touchscreen data. */
static int polling;
module_param(polling, int, 0);

/* Force to go on even if we fail to create i2c dummy devices. */
static int force_i2c_communication = 1;
module_param(force_i2c_communication, int, 0);

static int revert = true;
module_param(revert, int, 0);

static struct tp_priv global_tp;

static int intel_get_i2c_bus_id(int adapter_id, char *adapter_bdf, int bdf_len)
{
	struct i2c_adapter *adapter;
	struct device *parent;
	struct device *pp;
	int i = 0;
	int found = 0;
	int retry_count = 0;

	if (!adapter_bdf || bdf_len > 32)
		return -1;

	while (retry_count < 5) {
		i = 0;
		found = 0;
		while ((adapter = i2c_get_adapter(i)) != NULL) {
			parent = adapter->dev.parent;
			pp = parent->parent;
			i2c_put_adapter(adapter);
			pr_debug("[FPD_DP] dev_name(pp): %s\n", dev_name(pp));
			if (pp && !strncmp(adapter_bdf, dev_name(pp), bdf_len)) {
				found = 1;
				break;
			}
			i++;
		}

		if (found) {
			pr_debug("[FPD_DP] found dev_name(pp) %s\n", dev_name(pp));
			break;
		}
		retry_count++;
		pr_debug("[FPD_DP] not found retry_count %d\n", retry_count);
		msleep(1000);
	}

	if (found)
		return i;

	/* Not found */
	return -1;
}


static int get_bus_number(void)
{
	char adapter_bdf[32] = ADAPTER_PP_DEV_NAME;
	int bus_number = intel_get_i2c_bus_id(0, adapter_bdf, 32);
	return bus_number;
}


static int fpd_read_reg_force(struct i2c_adapter *adapter, u16 addr, u8 reg_addr, u8 *val)
{
	u8 buf[1];
	int ret = 0;

	struct i2c_msg msg[2];

	buf[0] = reg_addr & 0xff;

	msg[0].addr = addr;
	msg[0].flags = 0;
	msg[0].buf = &buf[0];
	msg[0].len = 1;

	msg[1].addr = addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = val;
	msg[1].len = 1;

	i2c_transfer(adapter, msg, 2);
	if (ret < 0) {
		pr_err("[TP] [-%s-%s-%d-], fail reg_addr=0x%x, val=%u\n",
			__FILE__, __func__, __LINE__, reg_addr, *val);
		return -ENODEV;
	}

	pr_debug("[TP] RIB 0x%02x: 0x%02x 0x%02x OK\n", addr, reg_addr, *val);
	return 0;
}


static int fpd_read_reg(struct i2c_client *client, u8 reg_addr, u8 *val)
{
	return fpd_read_reg_force(client->adapter, client->addr, reg_addr, val);
}


static int fpd_write_reg_force(struct i2c_adapter *adapter, u16 addr, u8 reg_addr, u8 val)
{
	int ret = 0;
	struct i2c_msg msg;
	u8 buf[2];

	buf[0] = reg_addr & 0xff;
	buf[1] = val;

	msg.addr = addr;
	msg.flags = 0;
	msg.buf = &buf[0];
	msg.len = 2;

	ret = i2c_transfer(adapter, &msg, 1);
	if (ret < 0) {
		pr_err("[TP] [-%s-%s-%d-], fail addr=0x%02x, reg_addr=0x%02x, val=0x%02x\n",
			__FILE__, __func__, __LINE__, addr, reg_addr, val);
		return ret;
	}
	pr_debug("[TP] WIB 0x%02x: 0x%02x 0x%02x OK\n", addr, reg_addr, val);
	return 0;
}


static int fpd_write_reg(struct i2c_client *client, u8 reg_addr, u8 val)
{
	return fpd_write_reg_force(client->adapter, client->addr, reg_addr, val);
}


/* Indirect read from SerDes */
static int fpd_read_reg_ind_force(struct i2c_adapter *adapter, u16 addr, u8 page, u8 reg_addr, u8 *val)
{
	int ret = 0;

	ret |= fpd_write_reg_force(adapter, addr, 0x40, (page << 2) | 0x01);
	ret |= fpd_write_reg_force(adapter, addr, 0x41, reg_addr);
	ret |= fpd_read_reg_force(adapter, addr, 0x42, val);
	return ret;
}


static int fpd_read_reg_ind(struct i2c_client *client, u8 page, u8 reg_addr, u8 *val)
{
	return fpd_read_reg_ind_force(client->adapter, client->addr, page, reg_addr, val);
}


/* Indirect write from SerDes */
static int fpd_write_reg_ind_force(struct i2c_adapter *adapter, u16 addr, u8 page, u8 reg_addr, u8 val)
{
	int ret = 0;

	ret |= fpd_write_reg_force(adapter, addr, 0x40, (page << 2) | 0x00);
	ret |= fpd_write_reg_force(adapter, addr, 0x41, reg_addr);
	ret |= fpd_write_reg_force(adapter, addr, 0x42, val);
	return ret;
}


static int fpd_write_reg_ind(struct i2c_client *client, u8 page, u8 reg_addr, u8 val)
{
	return fpd_write_reg_ind_force(client->adapter, client->addr, page, reg_addr, val);
}


int tp_get_mcu_tp_data(struct tp_priv *priv, struct tp_report_data *buff, u8 command)
{
	struct i2c_msg msg[2];
	const u16 mcu_addr = I2C_MCU_ADDRESS;
	struct i2c_adapter *adapter = priv->i2c_adap;

	u8 command_id[1] = { command };
	int ret = 0;

	msg[0].addr = mcu_addr;
	msg[0].flags = 0;
	msg[0].buf = command_id;
	msg[0].len = sizeof(command_id);

	msg[1].addr = mcu_addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = (u8 *) buff;
	msg[1].len = TP_PROTOCOL_DATA_LENGTH_TP_REPORT + TP_PROTOCOL_TRANSPORT_OVERHEAD;

	ret = i2c_transfer(adapter, msg, 2);
	if (ret < 0) {
		pr_err("Failed to transfer data\n");
		return -1;
	}

	return 0;
}


static int tp_ack_irq(struct tp_priv *priv)
{
	struct i2c_adapter *adapter = priv->i2c_adap;
	const u16 ser_addr = I2C_SER_ADDRESS;
	u8 value;
	int ret = 0;

	ret = fpd_read_reg_ind_force(adapter, ser_addr, 0x09, 0x8d, &value);
	pr_debug("Ser INTR_STS_FPD4_PORT0 0x8d = 0x%x\n", value);

	return ret;
}


static int process_tp_point_data(struct tp_point_data *data)
{
	u16 x, y;

	x = ((u16) data->x_h << 8) | data->x_l;
	y = ((u16) data->y_h << 8) | data->y_l;
	if (revert) {
		x = TP_WIDTH - x;
		y = TP_HEIGHT - y;
		data->x_h = x >> 8;
		data->x_l = x & 0x00ff;
		data->y_h = y >> 8;
		data->y_l = y & 0x00ff;
	}
	pr_debug("id = %u, status = %s, x = %u, y = %u\n",
		data->id, tp_status_str(data->status), x, y);
	return 0;
}


static void tp_irq_work(struct tp_priv *priv)
{
	struct tp_report_data report_data;
	u8 command_id = TP_PROTOCOL_COMMAND_ID_QUERY;
	int ret = 0, i, retries = 1;

	do {
		pr_debug("%s: Try to obtain tp data\n", __func__);

		ret = tp_get_mcu_tp_data(priv, &report_data, command_id);
		if (ret < 0) {
			pr_err("Failed to transfer data\n");
			return;
		}

		if (report_data.command_id != TP_PROTOCOL_COMMAND_ID_TP_REPORT
		    || report_data.data_length_l != TP_PROTOCOL_DATA_LENGTH_TP_REPORT) {
			pr_debug("Skip non-tp data\n");
			continue;
		}
		if (report_data.number_points == 0)
			pr_debug("number of points is zero\n");

		for (i = 0; i < report_data.number_points; ++i)
			process_tp_point_data(&report_data.point_data[i]);

		ret = tp_input_dev_report(priv, &report_data);
		if (ret < 0) {
			pr_err("Failed to report data to input dev\n");
			break;
		}
		--retries;
		command_id = TP_PROTOCOL_COMMAND_ID_TP_REPORT;
		tp_ack_irq(priv);
	} while (retries > 0);
}


static irqreturn_t tp_irq_handler(int irq, void *arg)
{
	struct tp_priv *priv = arg;

	pr_debug("%s: start IRQ handler, value = %d\n", __func__,
		gpiod_get_value(priv->tp_gpio));

	tp_irq_work(priv);
	return IRQ_HANDLED;
}


int tp_kthread_ack(void *data)
{
	struct tp_priv *priv = data;

	pr_debug("%s: kthread started\n", __func__);
	while (!kthread_should_stop()) {
		tp_ack_irq(priv);
		msleep(50);
	}
	pr_debug("%s: kthread stopped\n", __func__);
	return 0;
}


int tp_kthread_polling(void *data)
{
	struct tp_priv *priv = data;
	struct tp_report_data data1, data2, effective;
	struct tp_report_data *report_data;
	struct tp_report_data *prev = &data2, *curr = &data1;
	int ret = 0, i;
	int same_data_count = 0;
	bool need_report_relase = true;
	int same_data_threshold = 50;

	pr_debug("%s: kthread started\n", __func__);
	while (!kthread_should_stop()) {
		tp_ack_irq(priv);
		ret = tp_get_mcu_tp_data(priv, curr, TP_PROTOCOL_COMMAND_ID_TP_REPORT);
		memcpy(&effective, curr, sizeof(struct tp_report_data));
		report_data = (struct tp_report_data *) &effective;

		swap(curr, prev);
		if (report_data->command_id != TP_PROTOCOL_COMMAND_ID_TP_REPORT ||
		    report_data->data_length_l != TP_PROTOCOL_DATA_LENGTH_TP_REPORT) {
			pr_debug("Skip non-tp data, command_id = %u, length = %u\n",
				report_data->command_id, report_data->data_length_l);
			continue;
		}
		if (memcmp(curr, prev, sizeof(struct tp_report_data)) == 0) {
			pr_debug("Same data skipped\n");
			++same_data_count;
			if (need_report_relase && same_data_count == same_data_threshold) {
				for (i = 0; i < report_data->number_points; ++i) {
					report_data->point_data[i].status = TP_PROTOCOL_TP_STATUS_RELEASE;
					report_data->point_data[i].x_l = 0;
					report_data->point_data[i].x_h = 0;
					report_data->point_data[i].y_l = 0;
					report_data->point_data[i].y_h = 0;
					process_tp_point_data(&report_data->point_data[i]);
				}
				ret = tp_input_dev_report(priv, report_data);
				if (ret < 0)
					pr_err("Failed to report data to input dev\n");
				msleep(10);
			}
		} else {
			same_data_count = 0;
			need_report_relase = true;
			for (i = 0; i < report_data->number_points; ++i)
				process_tp_point_data(&report_data->point_data[i]);
			ret = tp_input_dev_report(priv, report_data);
			if (ret < 0)
				pr_err("Failed to report data to input dev\n");
			msleep(10);
		}
	}
	pr_debug("%s: kthread stopped\n", __func__);
	return 0;
}


static int tp_kthread_ack_create(struct tp_priv *priv)
{
	priv->ack_kthread = kthread_create(tp_kthread_ack, priv, "ef1e-tp-ack");
	if (!priv->ack_kthread) {
		pr_err("Failed to create kthread\n");
		return -ENOMEM;
	}
	wake_up_process(priv->ack_kthread);
	pr_debug("kthread tp-polling started\n");
	return 0;
}


static int tp_kthread_polling_create(struct tp_priv *priv)
{
	priv->polling_kthread = kthread_create(tp_kthread_polling, priv, "ef1e-tp-polling");
	if (!priv->polling_kthread) {
		pr_err("Failed to create kthread\n");
		return -ENOMEM;
	}
	wake_up_process(priv->polling_kthread);
	pr_debug("kthread tp-polling started\n");
	return 0;
}


static int tp_i2c_passthrough_init(struct tp_priv *priv)
{
	struct i2c_adapter *adapter = priv->i2c_adap;
	u16 ser_addr = I2C_SER_ADDRESS;
	int ret = 0;

	ret |= fpd_write_reg_force(adapter, ser_addr, 0x70, I2C_MCU_ADDRESS << 1);
	ret |= fpd_write_reg_force(adapter, ser_addr, 0x78, I2C_MCU_ADDRESS << 1);
	ret |= fpd_write_reg_force(adapter, ser_addr, 0x88, 0);
	ret |= fpd_write_reg_force(adapter, ser_addr, 0x07, 0x88);

	return ret;
}


static int tp_i2c_clock_init(struct tp_priv *priv)
{
	struct i2c_adapter *adapter = priv->i2c_adap;
	u16 addr = I2C_DES_ADDRESS;
	int ret = 0;

	ret |= fpd_write_reg_force(adapter, addr, 0x2b, 0x0a);
	ret |= fpd_write_reg_force(adapter, addr, 0x2c, 0x0b);

	return ret;
}


static struct gpiod_lookup_table tp_gpio_table = {
	.dev_id = NULL,
	.table = {
		/*
		 * These GPIOs are on the dm355evm_msp
		 * GPIO chip at index 0..7
		 */
		GPIO_LOOKUP_IDX("INTC1055:00", 86, "TP_IRQ", 86, GPIO_ACTIVE_LOW | GPIO_PULL_UP),
		{ },
	},
};


static int tp_gpio_irq_init(struct tp_priv *priv)
{
	int ret;

	gpiod_add_lookup_table(&tp_gpio_table);
	priv->tp_gpio = gpiod_get_index(NULL, "TP_IRQ", 86, GPIOD_IN);
	if (IS_ERR(priv->tp_gpio)) {
		pr_err("faied to get GPIO\n");
		return -ENODEV;
	}
	priv->tp_irq = gpiod_to_irq(priv->tp_gpio);
	if (priv->tp_irq <= 0) {
		pr_err("Failed to get IRQ\n");
		return -EBUSY;
	}
	pr_debug("TP irq = %d\n", priv->tp_irq);
	ret = request_threaded_irq(priv->tp_irq,
				   NULL, tp_irq_handler,
				   IRQF_TRIGGER_FALLING |
				   IRQF_ONESHOT,
				   "tp-irq", priv);
	if (ret) {
		pr_err("Failed to request edge IRQ for TP\n");
		return ret;
	}
	pr_debug("irq handler installed\n");

	ret = tp_kthread_ack_create(priv);
	return ret;
}


static void tp_gpio_irq_destroy(struct tp_priv *priv)
{
	if (priv->tp_irq > 0)
		free_irq(priv->tp_irq, priv);
	if (!IS_ERR_OR_NULL(priv->tp_gpio)) {
		gpio_free(desc_to_gpio(priv->tp_gpio));
		gpiod_remove_lookup_table(&tp_gpio_table);
	}
	if (priv->ack_kthread)
		kthread_stop(priv->ack_kthread);
}

static int tp_serdes_irq_init(struct tp_priv *priv)
{
	struct i2c_adapter *adapter = priv->i2c_adap;
	u16 ser_addr = I2C_SER_ADDRESS;
	u16 des_addr = I2C_DES_ADDRESS;
	int ret = 0;

	ret |= fpd_write_reg_force(adapter, ser_addr, 0x51, 0x83); /* INTERRUPT_CTL */
	ret |= fpd_write_reg_force(adapter, ser_addr, 0xc6, 0x21); /* FPD3_ICR */

	/* Set IE_DES_INT for INTR_CTL_FPD4_PORT0 in page 9 */
	ret |= fpd_write_reg_ind_force(adapter, ser_addr, 0x09, 0x8c, 0x30);

	/* Set IE_DES_INT for INTR_CTL_FPD4_PORT1 in page 9 */
	ret |= fpd_write_reg_ind_force(adapter, ser_addr, 0x09, 0x9c, 0x30);

	ret |= fpd_write_reg_force(adapter, des_addr, 0x44, 0x81);
	ret |= fpd_write_reg_force(adapter, des_addr, 0x45, 0x80);
	ret |= fpd_write_reg_force(adapter, des_addr, 0x52, 0x01);

	/* Set IC_INTB_IN_P0 for INTB_ICR_P0 and INTB_ICR_P1 in page 1*/
	/* IND_ACC_ADDR - INTB_ICR_P0 */
	ret |= fpd_write_reg_ind_force(adapter, des_addr, 0x01, 0x7e, 0x03);
	/* IND_ACC_ADDR - INTB_ICR_P1 */
	ret |= fpd_write_reg_ind_force(adapter, des_addr, 0x01, 0x7f, 0x03);

	return ret;
}


static void tp_priv_init(struct tp_priv *priv)
{
	memset(priv, 0, sizeof(struct tp_priv));
	priv->polling = polling;
}


static int __init tp_driver_init(void)
{
	int ret;
	int i2c_bus_number;
	struct i2c_adapter *i2c_adap;
	struct tp_priv *priv = &global_tp;

	tp_priv_init(priv);

	i2c_bus_number = get_bus_number();
	pr_info("i2c bus number = %d\n", i2c_bus_number);
	i2c_adap = i2c_get_adapter(i2c_bus_number);
	if (!i2c_adap) {
		pr_err("Cannot find a valid i2c bus for tp\n");
		ret = -ENODEV;
		goto error;
	}
	i2c_put_adapter(i2c_adap);
	priv->i2c_adap = i2c_adap;

	priv->i2c_mcu_client = i2c_new_dummy_device(priv->i2c_adap, I2C_MCU_ADDRESS);
	if (IS_ERR_OR_NULL(priv->i2c_mcu_client) && !force_i2c_communication) {
		pr_err("Failed to create dummy i2c device for MCU\n");
		ret = -EBUSY;
		goto error;
	}

	priv->i2c_ser_client = i2c_new_dummy_device(priv->i2c_adap, I2C_SER_ADDRESS);
	if (IS_ERR_OR_NULL(priv->i2c_ser_client) && !force_i2c_communication) {
		pr_err("Failed to create dummy i2c device for SER\n");
		ret = -EBUSY;
		goto error;
	}

	priv->i2c_des_client = i2c_new_dummy_device(priv->i2c_adap, I2C_DES_ADDRESS);
	if (IS_ERR_OR_NULL(priv->i2c_des_client) && !force_i2c_communication) {
		pr_err("Failed to create dummy i2c device for DES\n");
		ret = -EBUSY;
		goto error;
	}

	if (!IS_ERR_OR_NULL(priv->i2c_ser_client)) {
		ret = tp_i2c_passthrough_init(priv);
		if (ret) {
			pr_err("Failed to enable i2c passthrough\n");
			goto error;
		}
		pr_debug("i2c passthrough enabled\n");
	}

	ret = tp_i2c_clock_init(priv);
	if (ret)
		pr_warn("Failed to enable i2c fast mode\n");

	ret = tp_serdes_irq_init(priv);
	if (ret < 0) {
		pr_err("Failed to initialize TP IRQ\n");
		goto error;
	}
	/* Tick off the MCU to start reporting IRQ. */
	tp_ack_irq(priv);

	if (priv->polling)
		ret = tp_kthread_polling_create(priv);
	else
		ret = tp_gpio_irq_init(priv);
	if (ret < 0)
		goto error;

	ret = tp_input_dev_init(priv);
	if (ret < 0) {
		pr_err("Failed to initialize input device\n");
		goto error;
	}
	pr_debug("%s(): done\n", __func__);
	return 0;

error:
	if (priv->polling) {
		if (priv->polling_kthread)
			kthread_stop(priv->polling_kthread);
	} else {
		tp_gpio_irq_destroy(priv);
	}
	i2c_unregister_device(priv->i2c_des_client);
	i2c_unregister_device(priv->i2c_ser_client);
	i2c_unregister_device(priv->i2c_mcu_client);
	pr_err("Error occurring, initialization is aborted\n");
	return ret;
}


static void __exit tp_driver_exit(void)
{
	struct tp_priv *priv = &global_tp;

	if (priv->polling) {
		if (priv->polling_kthread)
			kthread_stop(priv->polling_kthread);
	} else {
		tp_gpio_irq_destroy(priv);
	}
	i2c_unregister_device(priv->i2c_des_client);
	i2c_unregister_device(priv->i2c_ser_client);
	i2c_unregister_device(priv->i2c_mcu_client);
	tp_input_dev_destroy(priv);

	pr_debug("TP driver removed\n");
}

static int __init tp_driver_init(void);
static void __exit tp_driver_exit(void);

module_init(tp_driver_init);
module_exit(tp_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Weifeng Liu <weifeng.liu@intel.com>");
MODULE_DESCRIPTION("EF1E touchscreen driver");

