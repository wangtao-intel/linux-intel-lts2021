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
#include <linux/platform_device.h>

#include "../../../gpu/drm/bridge/ti/fpd_dp_ser_drv.h"

#include "ef1e_tp.h"
#include "ef1e_tp_input.h"
#include "ef1e_tp_protocol.h"

#define ADAPTER_PP_DEV_NAME			"0000:00:15.1"

#define I2C_SER_ADDRESS				0x14
#define I2C_DES_ADDRESS				0x30
#define I2C_MCU_ADDRESS				0x78

#define EF1E_TP_PLATFORM_DEV_NAME		"ef1e_tp"

/* Set polling to non-zero to enable polling mode for touchscreen data. */
static int polling = 0;
module_param(polling, int, 0);

/* Force to go on even if we fail to create i2c dummy devices. */
static int force_i2c_communication = 1;
module_param(force_i2c_communication, int, 0);

static int revert = 1;
module_param(revert, int, 0);

static int ack_thread = 0;
module_param(ack_thread, int, 0);

static struct tp_priv global_tp;

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
		pr_err("failed to transfer data\n");
		return -1;
	}

	return 0;
}

/* Usually we don't care about the result this function returns, but who knows */
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
	int ret = 0, i;

	pr_debug("%s: try to obtain tp data\n", __func__);

	fpd_dp_ser_lock_global();

	if (!fpd_dp_ser_ready() || !READ_ONCE(priv->initialized)) {
		fpd_dp_ser_unlock_global();
		pr_info("%s: not ready to handle irq, ready = %d, "
			"initialized = %d\n",
			__func__,
			fpd_dp_ser_ready(),
			READ_ONCE(priv->initialized));
		return;
	}

	ret = tp_get_mcu_tp_data(priv, &report_data, command_id);
	if (ret < 0) {
		pr_err("%s: failed to get tp data\n", __func__);
		fpd_dp_ser_unlock_global();
		return;
	}
	tp_ack_irq(priv);
	fpd_dp_ser_unlock_global();

	if (report_data.command_id != TP_PROTOCOL_COMMAND_ID_TP_REPORT
		|| report_data.data_length_l != TP_PROTOCOL_DATA_LENGTH_TP_REPORT) {
		pr_debug("Skip non-tp data\n");
		return;
	}
	if (report_data.number_points == 0)
		pr_debug("number of points is zero\n");

	for (i = 0; i < report_data.number_points; ++i)
		process_tp_point_data(&report_data.point_data[i]);

	ret = tp_input_dev_report(priv, &report_data);
	if (ret < 0)
		pr_err("failed to report data to input dev\n");
}


static irqreturn_t tp_irq_handler(int irq, void *arg)
{
	struct tp_priv *priv = arg;

	pr_debug("%s: start IRQ handler, value = %d\n", __func__,
		gpiod_get_value(priv->tp_gpio));

	tp_irq_work(priv);
	return IRQ_HANDLED;
}


/*
 * We might accidentally miss interrupt from the MCU and if that happen, we
 * won't get informed of any interrupt any more.  To break out from this dead
 * lock, we periodically acknowledge the MCU.
 */
int tp_kthread_ack(void *data)
{
	struct tp_priv *priv = data;

	pr_debug("%s: kthread started\n", __func__);
	while (!kthread_should_stop()) {
		fpd_dp_ser_lock_global();
		if (fpd_dp_ser_ready() && READ_ONCE(priv->initialized))
			tp_ack_irq(priv);
		else
			pr_debug("%s: skip ack, ready = %d, initialized = %d\n",
				__func__, fpd_dp_ser_ready(),
				READ_ONCE(priv->initialized));
		fpd_dp_ser_unlock_global();
		msleep(500);
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
		fpd_dp_ser_lock_global();

		if (!fpd_dp_ser_ready()) {
			fpd_dp_ser_unlock_global();
			msleep(50);
			continue;
		}

		tp_ack_irq(priv);
		ret = tp_get_mcu_tp_data(priv, curr, TP_PROTOCOL_COMMAND_ID_TP_REPORT);
		fpd_dp_ser_unlock_global();

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
	priv->polling_kthread = kthread_create(tp_kthread_polling, priv,
					       "ef1e-tp-polling");
	if (!priv->polling_kthread) {
		pr_err("Failed to create kthread\n");
		return -ENOMEM;
	}
	wake_up_process(priv->polling_kthread);
	pr_debug("kthread tp-polling started\n");
	return 0;
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
		pr_err("failed to get IRQ\n");
		return -EBUSY;
	}
	pr_debug("TP irq = %d\n", priv->tp_irq);
	ret = request_threaded_irq(priv->tp_irq,
				   NULL, tp_irq_handler,
				   IRQF_TRIGGER_FALLING |
				   IRQF_ONESHOT,
				   "ef1e_tp-irq", priv);
	if (ret) {
		pr_err("Failed to request edge IRQ for TP\n");
		return ret;
	}
	pr_debug("irq handler installed\n");

	if (ack_thread) {
		ret = tp_kthread_ack_create(priv);
		if (ret) {
			pr_err("%s: failed to create ack thread\n", __func__);
			free_irq(priv->tp_irq, priv);
		}
	}

	return ret;
}


static void tp_gpio_irq_destroy(struct tp_priv *priv)
{
	if (priv->tp_irq > 0)
		free_irq(priv->tp_irq, priv);
	if (priv->ack_kthread)
		kthread_stop(priv->ack_kthread);
	if (!IS_ERR_OR_NULL(priv->tp_gpio)) {
		gpio_free(desc_to_gpio(priv->tp_gpio));
		gpiod_remove_lookup_table(&tp_gpio_table);
	}
}

/* Setup GPIO interrupt for serdes. */
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


static void tp_init_work(struct work_struct *work)
{
	struct tp_priv *priv = &global_tp;
	int ret;

retry:
	fpd_dp_ser_lock_global();
	if (!fpd_dp_ser_ready()) {
		fpd_dp_ser_unlock_global();
		pr_info("%s: not ready, wait for 50ms\n", __func__);
		msleep(50);
		goto retry;
	}

	msleep(5000);

	ret = tp_serdes_irq_init(priv);
	if (ret < 0) {
		fpd_dp_ser_unlock_global();
		pr_err("failed to initialize TP IRQ\n");
		return;
	}
	WRITE_ONCE(priv->initialized, true);
	pr_info("%s: set initialized to true\n", __func__);
	/* Tick off the MCU to start reporting IRQ. */
	tp_ack_irq(priv);
	fpd_dp_ser_unlock_global();
}


static void tp_priv_init(struct tp_priv *priv)
{
	memset(priv, 0, sizeof(struct tp_priv));
	priv->polling = polling;
	INIT_WORK(&priv->init_work, tp_init_work);
}


static int ef1e_tp_probe(struct platform_device *dev)
{
	int ret;
	int i2c_bus_number;
	struct i2c_adapter *i2c_adap;
	struct tp_priv *priv = &global_tp;

	i2c_bus_number = fpd_dp_ser_get_i2c_bus_number();
	i2c_adap = i2c_get_adapter(i2c_bus_number);
	if (!i2c_adap) {
		pr_err("cannot find a valid i2c bus for tp\n");
		ret = -ENODEV;
		goto error;
	}
	priv->i2c_adap = i2c_adap;

	priv->init_wq = create_workqueue("ef1e_tp-init-wq");
	if (IS_ERR_OR_NULL(priv->init_wq)) {
		dev_err(&dev->dev, "failed to create init wq\n");
		ret = -ENOMEM;
		goto error;
	}
	queue_work(priv->init_wq, &priv->init_work);

	ret = tp_input_dev_init(priv);
	if (ret < 0) {
		pr_err("Failed to initialize input device\n");
		goto error;
	}

	if (priv->polling)
		ret = tp_kthread_polling_create(priv);
	else
		ret = tp_gpio_irq_init(priv);
	if (ret < 0)
		goto error;

	pr_info("%s(): done\n", __func__);
	return 0;

error:
	if (priv->polling) {
		if (priv->polling_kthread)
			kthread_stop(priv->polling_kthread);
	} else {
		tp_gpio_irq_destroy(priv);
	}

	i2c_put_adapter(priv->i2c_adap);
	pr_err("error occured, initialization is aborted\n");
	return ret;
}

static int ef1e_tp_remove(struct platform_device *dev)
{
	struct tp_priv *priv = &global_tp;

	if (priv->polling) {
		if (priv->polling_kthread)
			kthread_stop(priv->polling_kthread);
	} else {
		tp_gpio_irq_destroy(priv);
	}
	destroy_workqueue(priv->init_wq);

	tp_input_dev_destroy(priv);
	i2c_put_adapter(priv->i2c_adap);

	pr_debug("ef1e_tp driver removed\n");
	return 0;
}


static int ef1e_tp_resume(struct device *dev)
{
	struct tp_priv *priv = &global_tp;
	int ret;

	queue_work(priv->init_wq, &priv->init_work);

	if (priv->polling)
		ret = tp_kthread_polling_create(priv);
	else
		ret = tp_gpio_irq_init(priv);

	return ret;
}


static int ef1e_tp_suspend(struct device *dev)
{
	struct tp_priv *priv = &global_tp;
	int ret;

	WRITE_ONCE(priv->initialized, 0);
	dev_info(dev, "%s: set initialized to false\n", __func__);
	if (priv->polling) {
		if (priv->polling_kthread)
			kthread_stop(priv->polling_kthread);
	} else {
		tp_gpio_irq_destroy(priv);
	}
	return 0;
}


static const struct dev_pm_ops ef1e_tp_pm_ops = {
	.suspend = ef1e_tp_suspend,
	.resume	= ef1e_tp_resume,
};

static struct platform_driver ef1e_tp_driver = {
	.probe = ef1e_tp_probe,
	.remove = ef1e_tp_remove,
	.driver = {
		.name = EF1E_TP_PLATFORM_DEV_NAME,
		.owner = THIS_MODULE,
		.pm = &ef1e_tp_pm_ops,
	},
};

static int __init tp_driver_init(void)
{
	int ret;
	struct tp_priv *priv = &global_tp;

	tp_priv_init(priv);

	priv->dev = platform_device_register_simple(EF1E_TP_PLATFORM_DEV_NAME,
							-1, NULL, 0);
	if (IS_ERR_OR_NULL(priv->dev)) {
		pr_err("failed to register platform device\n");
		ret = PTR_ERR(priv->dev);
		return ret;
	}
	ret = platform_driver_probe(&ef1e_tp_driver, ef1e_tp_probe);
	if (ret) {
		dev_err(&priv->dev->dev, "failed to probe driver\n");
		platform_device_unregister(priv->dev);
		return ret;
	}
	dev_info(&priv->dev->dev, "ef1e_tp driver loaded\n");
	return 0;
}


static void __exit tp_driver_exit(void)
{
	struct tp_priv *priv = &global_tp;

	platform_device_unregister(priv->dev);
	platform_driver_unregister(&ef1e_tp_driver);
}

static int __init tp_driver_init(void);
static void __exit tp_driver_exit(void);

module_init(tp_driver_init);
module_exit(tp_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Weifeng Liu <weifeng.liu@intel.com>");
MODULE_DESCRIPTION("EF1E touchscreen driver");

