// SPDX-License-Identifier: GPL-2.0-or-later
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

/*
 * Serializer: DS90Ux983-Q1
 * User Inputs:
 * Serializer I2C Address= 0x14
 * Max DP Lane Count = 4
 * Max DP Lane Rate = 2.7Gbps
 * DPRX no SSC Mode Enabled
 * DP SST Mode Enabled
 * DP Mode Enabled
 * FPD-Link Configuration: FPD-Link IV Single Port 0 - 6.75Gbps


 * Number of Displays = 1

 * Video Processor 0 (Stream 0) Properties:
 * Total Horizontal Pixels = 2720
 * Total Vertical Lines = 1481
 * Active Horizontal Pixels = 2560
 * Active Vertical Lines = 1440
 * Horizontal Back Porch = 80
 * Vertical Back Porch = 33
 * Horizontal Sync = 32
 * Vertical Sync = 5
 * Horizontal Front Porch = 48
 * Vertical Front Porch = 3
 * Horizontal Sync Polarity = Positive
 * Vertical Sync Polarity = Negative
 * Bits per pixel = 24
 * Pixel Clock = 241.5MHz
 * PATGEN Disabled

 * Deserializer 0: DS90Ux984-Q1
 * User Inputs:
 * Deserializer I2C Address = 0x30
 * Deserializer I2C Alias = 0x30
 * Override of DES eFuse enabled
 * DP Port 0 Enabled
 * DP0 Video Source = Serializer Stream 0
 * DP Port 0 PatGen Disabled
 * DP Port 1 Disabled
 * DP Port 1 PatGen Disabled
 * DP Rate set to 2.7 Gbps
 * DP lane number set to 4 lanes
 */

#include <linux/mutex.h>

#include "fpd_dp_ser_drv.h"

#define PAD_CFG_DW0_GPPC_A_16              0xfd6e0AA0

#ifdef DEBUG
#define fpd_dp_ser_debug	pr_info
#else
#define fpd_dp_ser_debug	pr_debug
#endif

static struct platform_device *pdev;
struct fpd_dp_ser_priv *fpd_dp_priv;
struct i2c_adapter *i2c_adap_mcu;
int deser_reset;

static bool deser_ready = false;
/*
 * Background: This module owns serdes and MCU, that is to say, serdes and MCU
 * would be initialized here.  However, EF1E touchscreen driver depends on the
 * functionality of serdes and MCU, so it can start to work only when
 * initiization is done in this module.  Thus there ought to be a way of
 * indicating whether the initializing process is complete, which is the intent
 * of this function.
 */
bool fpd_dp_ser_ready(void)
{
	return READ_ONCE(deser_ready);
}
EXPORT_SYMBOL_GPL(fpd_dp_ser_ready);

void fpd_dp_ser_set_ready(bool ready)
{
	pr_info("set ready to %d\n", ready);
	WRITE_ONCE(deser_ready, ready);
}
EXPORT_SYMBOL_GPL(fpd_dp_ser_set_ready);

static DEFINE_MUTEX(fpd_dp_ser_mutex);
/*
 * The following functions are required to call before/after any operation to
 * the serdes or MCU.  This is necessary to prevent resetting while another
 * module is still reading from or writing to them.
 */
void fpd_dp_ser_lock_global(void)
{
	mutex_lock(&fpd_dp_ser_mutex);
}
EXPORT_SYMBOL_GPL(fpd_dp_ser_lock_global);

void fpd_dp_ser_unlock_global(void)
{
	mutex_unlock(&fpd_dp_ser_mutex);
}
EXPORT_SYMBOL_GPL(fpd_dp_ser_unlock_global);

static struct i2c_board_info fpd_dp_i2c_board_info[] = {
	{
		I2C_BOARD_INFO("DS90UB983", FPD_DP_SER_TX_ADD),
	},
	{
		I2C_BOARD_INFO("DS90UB984", FPD_DP_SER_RX_ADD_A),
	},
	{
		I2C_BOARD_INFO("DS90UBMCU", FPD_DP_SER_MCU_ADD),
	},
};

char fpd_dp_ser_read_reg(struct i2c_client *client, u8 reg_addr, u8 *val)
{
	u8 buf[1];
	int ret = 0;

	struct i2c_msg msg[2];

	buf[0] = reg_addr & 0xff;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &buf[0];
	msg[0].len = 1;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = val;
	msg[1].len = 1;

	i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-], fail reg_addr=0x%x, val=%u\n",
				__FILE__, __func__, __LINE__, reg_addr, *val);
		return -ENODEV;
	}

	fpd_dp_ser_debug("[FPD_DP] RIB 0x%02x: 0x%02x 0x%02x OK\n", client->addr, reg_addr, *val);
	return 0;
}

bool fpd_dp_ser_write_reg(struct i2c_client *client, unsigned int reg_addr, u8 val)
{
	int ret = 0;
	struct i2c_msg msg;
	u8 buf[2];

	buf[0] = reg_addr & 0xff;
	buf[1] = val;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.buf = &buf[0];
	msg.len = 2;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0) {
		fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-], fail client->addr=0x%02x, reg_addr=0x%02x, val=0x%02x\n",
				__FILE__, __func__, __LINE__, client->addr, reg_addr, val);
		return false;
	}
	fpd_dp_ser_debug("[FPD_DP] WIB 0x%02x: 0x%02x 0x%02x OK\n",
			client->addr, reg_addr, val);
	return true;
}

bool fpd_dp_mcu_motor_mode(struct i2c_client *client, unsigned int reg_addr, u32 val)
{
	int ret = 0;
	int i = 0;
	struct i2c_msg msg;
	u8 buf[7];

	buf[0] = reg_addr & 0xff;
	buf[1] = 0x00;
	buf[2] = 0x04;
	buf[3] = (val & 0xff0000) >> 16;//0xff;
	buf[4] = (val & 0xff00) >> 8;
	buf[5] = val & 0xff;
	buf[6] = buf[0]^buf[1]^buf[2]^buf[3]^buf[4]^buf[5];

	msg.addr = client->addr;
	msg.flags = 0;
	msg.buf = &buf[0];
	msg.len = 7;

	ret = i2c_transfer(client->adapter, &msg, 1);

	if (ret < 0) {
		fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-], fail client->addr=0x%02x, reg_addr=0x%02x, val=0x%03x\n",
				__FILE__, __func__, __LINE__, msg.addr, reg_addr, val);
		return false;
	}

	for (i = 0; i < msg.len; i++) {
		fpd_dp_ser_debug("[FPD_DP] WIB 0x%02x: 0x%02x buf[%d] 0x%02x OK\n",
			msg.addr, reg_addr, i, buf[i]);
	}

	return true;
}

int fpd_dp_mcu_read_reg(struct i2c_client *client, unsigned int reg_addr, u8 len, u8 *data)
{
	struct i2c_msg msg[2];
	int i = 0;
	u8 buf[1];
	int ret = 0;

	buf[0] = reg_addr & 0xff;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &buf[0];
	msg[0].len = sizeof(buf);

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = data;
	msg[1].len = len;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		pr_err("Failed to transfer data\n");
		return -1;
	}

	return 0;
}

/*
 * TODO not used
 * this code is for check i2c return val
 */
static int fpd_dp_read_lock(struct i2c_client *client, unsigned int reg_addr,
		u32 mask, u32 expected_value)
{
	u8 reg_data;

	fpd_dp_ser_read_reg(client, reg_addr, &reg_data);
	if ((reg_data & mask) == expected_value)
		return 0;

	return -1;
}

void fpd_dp_ser_update(struct i2c_client *client,
		u32 reg, u32 mask, u8 val)
{
	u8 update_val;

	fpd_dp_ser_read_reg(client, reg, &update_val);
	update_val = ((update_val & (~mask)) | (val & mask));
	fpd_dp_ser_write_reg(client, reg, update_val);
}

/**
 * @brief reset
 * @param client 
 */
void fpd_dp_ser_reset(struct i2c_client *client)
{
	/* soft reset */
	fpd_dp_ser_write_reg(client, 0x01, 0xff);
	/* write port select to port 0 initially */
	fpd_dp_ser_write_reg(client, 0x2d, 0x01);

	/* Video Input Reset */
	fpd_dp_ser_write_reg(client, 0x49, 0x54);
	fpd_dp_ser_write_reg(client, 0x4a, 0x00);
	fpd_dp_ser_write_reg(client, 0x4b, 0x01);
	fpd_dp_ser_write_reg(client, 0x4c, 0x00);
	fpd_dp_ser_write_reg(client, 0x4d, 0x00);
	fpd_dp_ser_write_reg(client, 0x4e, 0x00);
}
/**
 * @brief Set up Variables
 * @param client
 */
void fpd_dp_ser_set_up_variables(struct i2c_client *client)
{
	/* i2c 400k */
	fpd_dp_ser_write_reg(client, 0x2b, 0x0a);
	fpd_dp_ser_write_reg(client, 0x2c, 0x0b);

	fpd_dp_ser_write_reg(client, 0x70, FPD_DP_SER_RX_ADD_A);
	fpd_dp_ser_write_reg(client, 0x78, FPD_DP_SER_RX_ADD_A);
	fpd_dp_ser_write_reg(client, 0x88, 0x0);
}

void  fpd_dp_ser_set_up_mcu(struct i2c_client *client)
{
	fpd_dp_ser_write_reg(client, 0x70, FPD_DP_SER_MCU_ADD << 1);
	fpd_dp_ser_write_reg(client, 0x78, FPD_DP_SER_MCU_ADD << 1);
	fpd_dp_ser_write_reg(client, 0x88, 0x0);
	fpd_dp_ser_write_reg(client, 0x07, 0x88);
}

void  fpd_dp_ser_motor_open(struct i2c_client *client)
{
	u8 read_motor_mode[7] = { 0 };
	u32 data_motor = 0;
	int i = 0;

	data_motor = 0x0050ff;
	fpd_dp_mcu_motor_mode(client, 0x23, data_motor);

	fpd_dp_mcu_read_reg(client, 0x63, 7, &read_motor_mode[0]);
	for (i = 0; i < 7; i++)
		fpd_dp_ser_debug("[FPD_DP] RIB [FPD_DP] RIB 0x78: 0x63, read_motor_mode[%d] 0x%02x OK\n",
			i, read_motor_mode[i]);
}

void  fpd_dp_ser_motor_close(struct i2c_client *client)
{
	u8 read_motor_mode[7] = { 0 };
	u32 data_motor = 0;
	int i = 0;

	data_motor = 0x0000ff;
	fpd_dp_mcu_motor_mode(client, 0x23, data_motor);

	fpd_dp_mcu_read_reg(client, 0x63, 7, &read_motor_mode[0]);
	for (i = 0; i < 7; i++)
		fpd_dp_ser_debug("[FPD_DP] RIB [FPD_DP] RIB 0x78: 0x63, read_motor_mode[%d] 0x%02x OK\n",
			i, read_motor_mode[i]);
}

/**
 * @brief Check MODE Strapping
 * @param client 
 */
void fpd_dp_ser_check_mode_strapping(struct i2c_client *client)
{
	u8 TX_MODE_STS;
	u8 GENERAL_CFG;
	u8 read_val;

	/* Check MODE Strapping */
	fpd_dp_ser_read_reg(client, 0x27, &read_val);
	TX_MODE_STS = read_val;

	if (TX_MODE_STS == 0)
		fpd_dp_ser_debug("Error: No Serializer Detected\n");

	fpd_dp_ser_read_reg(client, 0x7, &read_val);
	GENERAL_CFG = read_val;
	if ((GENERAL_CFG & 0x01) == 1) {
		fpd_dp_ser_debug("MODE Strapped for FPD III Mode\n");
		fpd_dp_priv->FPD4_Strap_Rate_P0 = 0;
		fpd_dp_priv->FPD4_Strap_Rate_P1 = 0;
	} else {
		if ((TX_MODE_STS & 0x0F) == 0x0F) {
			fpd_dp_ser_debug("MODE Strapped for FPD III Mode");
			fpd_dp_priv->FPD4_Strap_Rate_P0 = FPD4_Strap_Rate_0;
			fpd_dp_priv->FPD4_Strap_Rate_P1 = FPD4_Strap_Rate_0;
		}
		if (((TX_MODE_STS & 0x0F) == 0x08) || (TX_MODE_STS & 0x0F) == 0x09) {
			fpd_dp_ser_debug("MODE Strapped for FPD IV 10.8Gbps");
			fpd_dp_priv->FPD4_Strap_Rate_P0 = FPD4_Strap_Rate_10_8;
			fpd_dp_priv->FPD4_Strap_Rate_P1 = FPD4_Strap_Rate_10_8;
		}
		if (((TX_MODE_STS & 0x0F) == 0x0A || (TX_MODE_STS & 0x0F) == 0x0B)) {
			fpd_dp_ser_debug("MODE Strapped for FPD IV 13.5Gbps\n");
			fpd_dp_priv->FPD4_Strap_Rate_P0 = FPD4_Strap_Rate_13_5;
			fpd_dp_priv->FPD4_Strap_Rate_P1 = FPD4_Strap_Rate_13_5;
		}
		if (((TX_MODE_STS & 0x0F) == 0x0C || (TX_MODE_STS & 0x0F) == 0x0D)) {
			fpd_dp_ser_debug("MODE Strapped for FPD IV 6.75Gbps\n");
			fpd_dp_priv->FPD4_Strap_Rate_P0 = FPD4_Strap_Rate_6_75;
			fpd_dp_priv->FPD4_Strap_Rate_P1 = FPD4_Strap_Rate_6_75;
		}
		if ((TX_MODE_STS & 0x0F) == 0x0E) {
			fpd_dp_ser_debug("MODE Strapped for FPD IV 3.375Gbps\n");
			fpd_dp_priv->FPD4_Strap_Rate_P0 = FPD4_Strap_Rate_3_375;
			fpd_dp_priv->FPD4_Strap_Rate_P1 = FPD4_Strap_Rate_3_375;
		}
	}
}

/**
 * Program SER to FPD-Link IV mode
 */
int fpd_dp_ser_program_fpd_4_mode(struct i2c_client *client)
{
	/* Disable FPD3 FIFO pass through */
	fpd_dp_ser_write_reg(client, 0x5b, 0x23);
	/* Force FPD4_TX single port 0 mode */
	fpd_dp_ser_write_reg(client, 0x05,0x2c);
	return 0;
}

/**
 * Set up FPD IV PLL Settings
 */
int fpd_dp_set_fpd_4_pll(struct i2c_client *client)
{
	/* Disable PLL0 */
	fpd_dp_ser_write_reg(client, 0x40, 0x08);
	fpd_dp_ser_write_reg(client, 0x41, 0x1b);
	fpd_dp_ser_write_reg(client, 0x42, 0x08);

	/* Disable PLL1 */
	fpd_dp_ser_write_reg(client, 0x40, 0x08);
	fpd_dp_ser_write_reg(client, 0x41, 0x5b);
	fpd_dp_ser_write_reg(client, 0x42, 0x08);
	/* Enable mode overwrite*/
	fpd_dp_ser_write_reg(client, 0x2, 0xd1);
	fpd_dp_ser_write_reg(client, 0x2d, 0x1);

	/* Select PLL reg page */
	fpd_dp_ser_write_reg(client, 0x40, 0x08);
	/* Select Ncount Reg */
	fpd_dp_ser_write_reg(client, 0x41, 0x05);
	/* Set Ncount */
	fpd_dp_ser_write_reg(client, 0x42, 0x64);
	/* Select post div reg */
	fpd_dp_ser_write_reg(client, 0x41, 0x13);
	/* Set post div for 6.75 Gbps */
	fpd_dp_ser_write_reg(client, 0x42, 0x80);
	/* Select write reg to port 0 */
	fpd_dp_ser_write_reg(client, 0x2d, 0x01);
	/* set BC sampling rate */
	fpd_dp_ser_write_reg(client, 0x6a, 0x0a);
	/* set BC fractional sampling */
	fpd_dp_ser_write_reg(client, 0x6e, 0x86);
	/* Select FPD page and set BC settings for FPD IV port 0 */
	fpd_dp_ser_write_reg(client, 0x40, 0x04);

	fpd_dp_ser_write_reg(client, 0x41, 0x06);
	fpd_dp_ser_write_reg(client, 0x42, 0x00);
	fpd_dp_ser_write_reg(client, 0x41, 0x0d);
	fpd_dp_ser_write_reg(client, 0x42, 0x34);
	fpd_dp_ser_write_reg(client, 0x41, 0x0e);
	fpd_dp_ser_write_reg(client, 0x42, 0x53);

	/* Set HALFRATE_MODE Override */
	fpd_dp_ser_write_reg(client, 0x2, 0x11);
	/* Set HALFRATE_MODE */
	fpd_dp_ser_write_reg(client, 0x2, 0x51);
	/* Unset HALFRATE_MODE Override */
	fpd_dp_ser_write_reg(client, 0x2, 0x50);


	/* Zero out fractional PLL for port 0 */
	fpd_dp_ser_write_reg(client, 0x40, 0x08);
	fpd_dp_ser_write_reg(client, 0x41, 0x04);
	fpd_dp_ser_write_reg(client, 0x42, 0x01);
	fpd_dp_ser_write_reg(client, 0x41, 0x1e);
	fpd_dp_ser_write_reg(client, 0x42, 0x0);
	fpd_dp_ser_write_reg(client, 0x41, 0x1f);
	fpd_dp_ser_write_reg(client, 0x42, 0x00);
	fpd_dp_ser_write_reg(client, 0x41, 0x20);
	fpd_dp_ser_write_reg(client, 0x42, 0x0);

#if 0
	fpd_dp_ser_write_reg(client, 0x41, 0x19);
	fpd_dp_ser_write_reg(client, 0x42, 0xff);
	fpd_dp_ser_write_reg(client, 0x41, 0x1a);
	fpd_dp_ser_write_reg(client, 0x42, 0xff);
	fpd_dp_ser_write_reg(client, 0x41, 0x1e);
	fpd_dp_ser_write_reg(client, 0x42, 0x00);
	fpd_dp_ser_write_reg(client, 0x41, 0x1f);
	fpd_dp_ser_write_reg(client, 0x42, 0x00);
	fpd_dp_ser_write_reg(client, 0x41, 0x20);
	fpd_dp_ser_write_reg(client, 0x42, 0x00);
#endif
	return 0;
}


/**
 * @brief fpd_dp_ser_enable_I2C_passthrough
 * @param client 
 */
void fpd_dp_ser_enable_I2C_passthrough(struct i2c_client *client)
{
	u8 read_val;
	u8 I2C_PASS_THROUGH;
	u8 I2C_PASS_THROUGH_MASK;
	u8 I2C_PASS_THROUGH_REG;

	fpd_dp_ser_debug("[FPD_DP] Enable I2C Passthrough\n");

	fpd_dp_ser_read_reg(client, 0x7, &read_val);
	I2C_PASS_THROUGH = read_val;
	I2C_PASS_THROUGH_MASK = 0x08;
	I2C_PASS_THROUGH_REG = I2C_PASS_THROUGH | I2C_PASS_THROUGH_MASK;
	/* Enable I2C Passthrough */
	fpd_dp_ser_write_reg(client, 0x07, I2C_PASS_THROUGH_REG);
}

/**
 * Configure and Enable PLLs
 */
int fpd_dp_ser_configue_enable_plls(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Configure and Enable PLLs\n");
	fpd_dp_ser_debug("[FPD_DP] Set VCO\n");
	/* Select PLL page */
	fpd_dp_ser_write_reg(client, 0x40,0x08);
	/* Select VCO reg */
	fpd_dp_ser_write_reg(client, 0x41,0x0e);
	/* Set VCO */
	fpd_dp_ser_write_reg(client, 0x42,0xc7);

	fpd_dp_ser_debug("[FPD_DP] reset PLL\n");
	/* soft reset PLL */
	fpd_dp_ser_write_reg(client, 0x01,0x30);


	fpd_dp_ser_debug("[FPD_DP] Enable PLL0\n");
	/* Select PLL page */
	fpd_dp_ser_write_reg(client, 0x40,0x08);
	fpd_dp_ser_write_reg(client, 0x41,0x1b);
	/* Enable PLL0 */
	fpd_dp_ser_write_reg(client, 0x42,0x00);


	/* soft reset Ser */
	fpd_dp_ser_write_reg(client, 0x01,0x01);
	usleep_range(20000, 22000);

	return 0;
}

/**
 * Soft reset Des
 */
int fpd_dp_deser_soft_reset(struct i2c_client *client)
{
	usleep_range(20000, 22000);
	u8 des_read = 0;

	/* Soft reset Des */
	if (!fpd_dp_priv->priv_dp_client[1])
		fpd_dp_priv->priv_dp_client[1] = i2c_new_dummy_device(fpd_dp_priv->i2c_adap, fpd_dp_i2c_board_info[1].addr);


	if (fpd_dp_priv->priv_dp_client[1] != NULL) {
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[1], 0x01, 0x01);
		usleep_range(20000, 22000);
		fpd_dp_ser_read_reg(fpd_dp_priv->priv_dp_client[1], 0x1, &des_read);
		des_read = 0;
		fpd_dp_ser_read_reg(fpd_dp_priv->priv_dp_client[1], 0x2, &des_read);
		des_read = 0;
		fpd_dp_ser_read_reg(fpd_dp_priv->priv_dp_client[1], 0x3, &des_read);
	}

	/* Select write to port0 reg */
	fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x2d, 0x01);
	return 0;
}

/**
 * @brief Set DP Config
 * @param client 
 * @return int 
 */
int fpd_dp_ser_set_dp_config(struct i2c_client *client)
{
	/* Enable APB Interface */
	fpd_dp_ser_write_reg(client, 0x48, 0x1);

	/* Force HPD low to configure 983 DP settings */
	fpd_dp_ser_write_reg(client, 0x49, 0x0);
	fpd_dp_ser_debug("[FPD_DP] Pull HPD low to configure DP settings\n");
	fpd_dp_ser_write_reg(client, 0x4a, 0x0);
	fpd_dp_ser_write_reg(client, 0x4b, 0x0);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	/* Set max advertised link rate = 2.7Gbps */
	fpd_dp_ser_write_reg(client, 0x49, 0x74);
	fpd_dp_ser_write_reg(client, 0x4a, 0x0);
	fpd_dp_ser_write_reg(client, 0x4b, 0xa);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	/* Set max advertised lane count = 4 */
	fpd_dp_ser_write_reg(client, 0x49, 0x70);
	fpd_dp_ser_write_reg(client, 0x4a, 0x0);
	fpd_dp_ser_write_reg(client, 0x4b, 0x4);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	/* Request min VOD swing of 0x02 */
	fpd_dp_ser_write_reg(client, 0x49, 0x14);
	fpd_dp_ser_write_reg(client, 0x4a, 0x2);
	fpd_dp_ser_write_reg(client, 0x4b, 0x2);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	/* Set SST/MST mode and DP/eDP Mode */
	fpd_dp_ser_write_reg(client, 0x49, 0x18);
	fpd_dp_ser_write_reg(client, 0x4a, 0x0);
	fpd_dp_ser_write_reg(client, 0x4b, 0x14);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	/* Force HPD high to trigger link training */
	fpd_dp_ser_write_reg(client, 0x49, 0x0);
	fpd_dp_ser_debug("[FPD_DP] Pull HPD High to start link training\n");
	fpd_dp_ser_write_reg(client, 0x4a, 0x0);
	fpd_dp_ser_write_reg(client, 0x4b, 0x1);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	return 0;
}

/**
 * @brief Program VP Configs
 * @param client 
 * @return int 
 */
int fpd_dp_ser_program_vp_configs(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Configure Video Processors\n");
	/* Configure VP 0 */
	fpd_dp_ser_write_reg(client, 0x40, 0x32);
	fpd_dp_ser_write_reg(client, 0x41, 0x01);
	/* Set VP_SRC_SELECT to Stream 0 for SST Mode */
	fpd_dp_ser_write_reg(client, 0x42, 0xa8);
	fpd_dp_ser_write_reg(client, 0x41, 0x02);
	/* VID H Active */
	fpd_dp_ser_write_reg(client, 0x42, 0x40);
	/* VID H Active */
	fpd_dp_ser_write_reg(client, 0x42, 0x0b);
	fpd_dp_ser_write_reg(client, 0x41, 0x10);
	/* Horizontal Active */
	fpd_dp_ser_write_reg(client, 0x42, 0x40);
	fpd_dp_ser_write_reg(client, 0x42, 0x0b);
	/* Horizontal Back Porch */
	fpd_dp_ser_write_reg(client, 0x42, 0xe0);
	fpd_dp_ser_write_reg(client, 0x42, 0x01);
	/* Horizontal Sync */
	fpd_dp_ser_write_reg(client, 0x42, 0x20);
	fpd_dp_ser_write_reg(client, 0x42, 0x00);
	/* Horizontal Total */
	fpd_dp_ser_write_reg(client, 0x42, 0x70);
	fpd_dp_ser_write_reg(client, 0x42, 0x0d);
	/* Vertical Active */
	fpd_dp_ser_write_reg(client, 0x42, 0x54);
	fpd_dp_ser_write_reg(client, 0x42, 0x06);
	/* Vertical Back Porch */
	fpd_dp_ser_write_reg(client, 0x42, 0x0c);
	fpd_dp_ser_write_reg(client, 0x42, 0x0);
	/* Vertical Sync */
	fpd_dp_ser_write_reg(client, 0x42, 0x08);
	fpd_dp_ser_write_reg(client, 0x42, 0x00);
	/* Vertical Front Porch */
	fpd_dp_ser_write_reg(client, 0x42, 0x08);
	fpd_dp_ser_write_reg(client, 0x42, 0x00);

	fpd_dp_ser_write_reg(client, 0x41, 0x27);

	/* HSYNC Polarity = +, VSYNC Polarity = - */
	fpd_dp_ser_write_reg(client, 0x42, 0x0);

	/* M/N Register */
	fpd_dp_ser_write_reg(client, 0x41, 0x23);
	/* M value */
	fpd_dp_ser_write_reg(client, 0x42, 0x50);
	fpd_dp_ser_write_reg(client, 0x42, 0x28);
	/* N value */
	fpd_dp_ser_write_reg(client, 0x42, 0x0f);
	return 0;
}

/**
 * @brief Enable VPs
 * @param client 
 * @return int 
 */
int fpd_dp_ser_enable_vps(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Enable Video Processors\n");

	/* Enable video processors*/
	/* Set number of VPs used = 1 */
	fpd_dp_ser_write_reg(client, 0x43, 0x00);
	fpd_dp_ser_write_reg(client, 0x44, 0x01);

	return 0;
}

/**
 * @brief Clear CRC errors from initial link process
 * @param client 
 * @return int 
 */
int fpd_dp_ser_clear_crc_error(struct i2c_client *client)
{
	u8 Reg_value;

	fpd_dp_ser_debug("[FPD_DP] Clear CRC errors from initial link process\n");

	fpd_dp_ser_read_reg(client, 0x2, &Reg_value);
	Reg_value = Reg_value | 0x20;
	/* CRC Error Reset */
	fpd_dp_ser_write_reg(client, 0x2, Reg_value);

	fpd_dp_ser_read_reg(client, 0x2, &Reg_value);
	Reg_value = Reg_value & 0xdf;
	/* CRC Error Reset Clear */
	fpd_dp_ser_write_reg(client, 0x2, Reg_value);

	fpd_dp_ser_write_reg(client, 0x2d, 0x1);
	usleep_range(20000, 22000);

	return 0;
}

void fpd_dp_deser_enable(void);
int fpd_dp_ser_configure_serializer_tx_link_layer(struct i2c_client *client);

/**
 * @brief Check if VP is synchronized to DP input
 * @param work 
 */
static void fpd_poll_training_lock(struct work_struct *work)
{
	u8 PATGEN_VP0 = 0;
	u8 VP0sts = 0;
	int retry = 0;

	fpd_dp_ser_debug("[FPD_DP] Check if VP is synchronized to DP input\n");

	/* Delay for VPs to sync to DP source */
	usleep_range(20000, 22000);

	/* Select VP Page */
	fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x40, 0x31);
	fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x41, 0x28);
	fpd_dp_ser_read_reg(fpd_dp_priv->priv_dp_client[0], 0x42, &PATGEN_VP0);
	fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x41, 0x30);
	fpd_dp_ser_read_reg(fpd_dp_priv->priv_dp_client[0], 0x42, &VP0sts);
	fpd_dp_ser_debug("[FPD_DP] VP0sts = 0x%02x\n", (VP0sts & 0x01));

	while (((VP0sts & 0x01) == 0) && retry < 10 && ((PATGEN_VP0 & 0x01) == 0)) {
		fpd_dp_ser_debug("[FPD_DP] VP0 Not Synced - Delaying 100ms. Retry = %d\n", retry);
		usleep_range(20000, 22000);
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x41, 0x30);
		fpd_dp_ser_read_reg(fpd_dp_priv->priv_dp_client[0], 0x42, &VP0sts);
		retry = retry + 1;
	}

	if (((VP0sts & 0x01) == 0)) {
		fpd_dp_ser_debug("[FPD_DP]  VPs not synchronized - performing video input reset\n");
		/* Video Input Reset if VP is not synchronized */
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x49, 0x54);
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x4a, 0x0);
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x4b, 0x1);
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x4c, 0x0);
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x4d, 0x0);
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x4e, 0x0);
//		goto reschedule;
	}

	fpd_dp_ser_debug("[FPD_DP] ser training lock completed, count = %d\n", fpd_dp_priv->count);
	fpd_dp_priv->count = 0;

	fpd_dp_ser_configure_serializer_tx_link_layer(fpd_dp_priv->priv_dp_client[0]);
	fpd_dp_ser_clear_crc_error(fpd_dp_priv->priv_dp_client[0]);
//	fpd_dp_deser_enable();

	return;

reschedule:
	fpd_dp_priv->count++;
	if (fpd_dp_priv->count > 10) {
		fpd_dp_ser_debug("[FPD_DP] ser training lock failed, count = %d\n", fpd_dp_priv->count);
		VP0sts = 0;
		fpd_dp_ser_read_reg(fpd_dp_priv->priv_dp_client[0], 0xC, &VP0sts);
		fpd_dp_ser_debug("[FPD_DP] ser training STS  %d\n", VP0sts);
		return;
	}

	queue_delayed_work(fpd_dp_priv->wq, &fpd_dp_priv->delay_work, msecs_to_jiffies(100));
}

/**
 * @brief 
 * @param client 
 * @return int 
 */
int fpd_dp_ser_configure_serializer_tx_link_layer(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Configure serializer TX link layer\n");
	/* Link layer Reg page */
	fpd_dp_ser_write_reg(client, 0x40, 0x2e);
	/* Link layer 0 stream enable */
	fpd_dp_ser_write_reg(client, 0x41, 0x01);
	/* Link layer 0 stream enable */
	fpd_dp_ser_write_reg(client, 0x42, 0x01);
	/* Link layer 0 time slot 0 */
	fpd_dp_ser_write_reg(client, 0x41, 0x06);
	/* Link layer 0 time slot */
	fpd_dp_ser_write_reg(client, 0x42, 0x41);
	/* Set Link layer vp bpp */
	fpd_dp_ser_write_reg(client, 0x41, 0x20);
	/* Set Link layer vp bpp according to VP Bit per pixel */
	fpd_dp_ser_write_reg(client, 0x42, 0x61);
	/* Link layer 0 enable */
	fpd_dp_ser_write_reg(client, 0x41, 0x00);
	/* Link layer 0 enable */
	fpd_dp_ser_write_reg(client, 0x42, 0x03);
	return 0;
}

/**
 * @brief Override DES 0 eFuse
 * @param client 
 */
int fpd_dp_deser_override_efuse(struct i2c_client *client)
{
	u8 DES_READBACK;

	fpd_dp_ser_read_reg(client, 0x0, &DES_READBACK);
	if (DES_READBACK == 0)
		fpd_dp_ser_debug("[FPD_DP] Error - no DES detected\n");
	else
		fpd_dp_ser_debug("[FPD_DP] Deserializer detected successfully\n");

	fpd_dp_ser_write_reg(client, 0x49, 0xc);
	fpd_dp_ser_write_reg(client, 0x4a, 0x0);
	fpd_dp_ser_write_reg(client, 0x48, 0x1b);
	fpd_dp_ser_read_reg(client, 0x4b, &DES_READBACK);
	if (DES_READBACK != 0x19) {
		fpd_dp_ser_debug("[FPD_DP] error\n");
	}
	usleep_range(20000, 22000);
	return 0;
}

/**
 * @brief Read Deserializer 0 Temp
 * @param client 
 */
void fpd_dp_deser_set_up_des_temp(struct i2c_client *client)
{
	u8 TEMP_FINAL;
	int TEMP_FINAL_C;
	int Efuse_TS_CODE = 2;
	int Ramp_UP_Range_CODES_Needed;
	int Ramp_DN_Range_CODES_Needed;
	int Ramp_UP_CAP_DELTA;
	int Ramp_DN_CAP_DELTA;
	int TS_CODE_UP;
	int TS_CODE_DN;
	u8 rb;


	fpd_dp_ser_debug("[FPD_DP] Configure deserializer 0 temp ramp optimizations\n");
	fpd_dp_ser_write_reg(client, 0x40, 0x6c);
	fpd_dp_ser_write_reg(client, 0x41, 0x0d);
	fpd_dp_ser_write_reg(client, 0x42, 0x00);
	fpd_dp_ser_write_reg(client, 0x41, 0x13);
	fpd_dp_ser_read_reg(client, 0x42, &TEMP_FINAL);
	TEMP_FINAL_C = 2*TEMP_FINAL - 273;
	fpd_dp_ser_debug("[FPD_DP] Deserializer 0 starting temp = %dC\n", TEMP_FINAL_C);

	Efuse_TS_CODE = 2;
	Ramp_UP_Range_CODES_Needed = (int)(((150-TEMP_FINAL_C)/(190/11)) + 1);
	Ramp_DN_Range_CODES_Needed = (int)(((TEMP_FINAL_C-30)/(190/11)) + 1);
	Ramp_UP_CAP_DELTA = Ramp_UP_Range_CODES_Needed - 4;
	Ramp_DN_CAP_DELTA = Ramp_DN_Range_CODES_Needed - 7;

	fpd_dp_ser_write_reg(client, 0x40, 0x3c);
	fpd_dp_ser_write_reg(client, 0x41, 0xf5);
	/* Override TS_CODE Efuse Code */
	fpd_dp_ser_write_reg(client, 0x42, (Efuse_TS_CODE<<4)+1);
	if (Ramp_UP_CAP_DELTA > 0) {
		fpd_dp_ser_debug("[FPD_DP] Adjusting ramp up and resetting DES\n");
		TS_CODE_UP = Efuse_TS_CODE - Ramp_UP_CAP_DELTA;
		if (TS_CODE_UP < 0) 
			TS_CODE_UP = 0;

		fpd_dp_ser_write_reg(client, 0x41, 0xf5);
		fpd_dp_ser_read_reg(client, 0x42, &rb);
		rb &= 0x8F;
		rb |= (TS_CODE_UP << 4);
		fpd_dp_ser_write_reg(client, 0x42, rb);
		fpd_dp_ser_read_reg(client, 0x42, &rb);
		rb &= 0xFE;
		rb |= 0x01;
		fpd_dp_ser_write_reg(client, 0x42, rb);
		fpd_dp_ser_write_reg(client, 0x01, 0x01);
		usleep_range(20000, 22000);
	}
	if (Ramp_DN_CAP_DELTA > 0) {
		fpd_dp_ser_debug("[FPD_DP] Adjusting ramp up and resetting DES\n");
		TS_CODE_DN = Efuse_TS_CODE + Ramp_DN_CAP_DELTA;
		if(TS_CODE_DN >= 7) 
			TS_CODE_DN = 7;

		fpd_dp_ser_write_reg(client, 0x41, 0xf5);
		fpd_dp_ser_read_reg(client, 0x42, &rb);
		rb &= 0x8F;
		rb |= (TS_CODE_DN << 4);
		fpd_dp_ser_write_reg(client, 0x42, rb);
		fpd_dp_ser_read_reg(client, 0x42, &rb);
		rb &= 0xFE;
		rb |= 0x01;
		fpd_dp_ser_write_reg(client, 0x42, rb);
		fpd_dp_ser_write_reg(client, 0x01, 0x01);
		usleep_range(20000, 22000);
	}
}

/**
 * @brief Hold Des DTG in reset
 * @param client 
 */
void fpd_dp_deser_hold_dtg_reset(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Hold Des 0 DTG in reset and configure video settings\n");
	/* Select DTG Page */
	fpd_dp_ser_write_reg(client, 0x40, 0x50);
	fpd_dp_ser_write_reg(client, 0x41, 0x32);
	/* Hold Local Display Output Port 0 DTG in Reset */
	fpd_dp_ser_write_reg(client, 0x42, 0x6);
	fpd_dp_ser_write_reg(client, 0x41, 0x62);
	/* Hold Local Display Output Port 1 DTG in Reset */
	fpd_dp_ser_write_reg(client, 0x42, 0x6);
}

/**
 * @brief Disable Stream Mapping
 * 
 * @param client 
 */
void fpd_dp_deser_disalbe_stream_mapping(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Disable Stream Mapping\n");
	/* Select both Output Ports */
	fpd_dp_ser_write_reg(client, 0xe, 0x3);
	/* Disable FPD4 video forward to Output Port */
	fpd_dp_ser_write_reg(client, 0xd0, 0x0);
	/* Disable FPD3 video forward to Output Port */
	fpd_dp_ser_write_reg(client, 0xd7, 0x0);
}

/**
 * @brief Setup DP ports
 * 
 * @param client 
 */
void fpd_dp_deser_setup_ports(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Setup DP ports\n");
	/* Select Port 1 registers */
	fpd_dp_ser_write_reg(client, 0xe, 0x12);
	/* Disable DP Port 1 */
	fpd_dp_ser_write_reg(client, 0x46, 0x0);
	/* Select Port 0 registers */
	fpd_dp_ser_write_reg(client, 0xe, 0x1);
	/* DP-TX-PLL RESET Applied */
	fpd_dp_ser_write_reg(client, 0x1, 0x40);
}

/**
 * @brief Force DP Rate
 * 
 * @param client 
 */
void fpd_dp_deser_force_dp_rate(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Force DP Rate\n");
	/* Select DP Page */
	fpd_dp_ser_write_reg(client, 0x40, 0x2c);
	fpd_dp_ser_write_reg(client, 0x41, 0x81);
	/* Set DP Rate to 2.7Gbps */
	fpd_dp_ser_write_reg(client, 0x42, 0x60);
	fpd_dp_ser_write_reg(client, 0x41, 0x82);
	/* Enable force DP rate with calibration disabled */
	fpd_dp_ser_write_reg(client, 0x42, 0x03);
	/* Select DP Page */
	fpd_dp_ser_write_reg(client, 0x40, 0x2c);
	fpd_dp_ser_write_reg(client, 0x41, 0x91);
	/* Force 4 lanes */
	fpd_dp_ser_write_reg(client, 0x42, 0xc);
	/* Disable DP SSCG */
	fpd_dp_ser_write_reg(client, 0x40, 0x30);
	fpd_dp_ser_write_reg(client, 0x41, 0xf);
	fpd_dp_ser_write_reg(client, 0x42, 0x1);
	fpd_dp_ser_write_reg(client, 0x1, 0x40);
}


/**
 * @brief Map video to display output
 * 
 * @param client 
 */
void fpd_dp_deser_map_output(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Map video to display output\n");
	/* Select both Output Ports */
	fpd_dp_ser_write_reg(client, 0x0e, 0x03);
	/* Enable FPD_RX video forward to Output Port */
	fpd_dp_ser_write_reg(client, 0xd0, 0x0c);
	/* Every stream forwarded on DC */
	fpd_dp_ser_write_reg(client, 0xd1, 0x0f);
	/* Send Stream 0 to Output Port 0 and Send Stream 1 to Output Port 1 */
	fpd_dp_ser_write_reg(client, 0xd6, 0x08);
	/* FPD3 to local display output mapping disabled */
	fpd_dp_ser_write_reg(client, 0xd7, 0x00);
	/* Select Port 0 */
	fpd_dp_ser_write_reg(client, 0x0e, 0x01);
}

/**
 * @brief Program quad pixel clock for DP port 0
 * 
 * @param client 
 */
void fpd_dp_deser_prog_pclk(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Program quad pixel clock for DP port 0\n");
	/* Select Port0 registers */
	fpd_dp_ser_write_reg(client, 0xe, 0x1);
	/* Enable clock divider */
	fpd_dp_ser_write_reg(client, 0xb1, 0x1);
	/* Program M value lower byte */
	fpd_dp_ser_write_reg(client, 0xb2, 0x5c);
	/* Program M value middle byte */
	fpd_dp_ser_write_reg(client, 0xb3, 0xaf);
	/* Program M value upper byte,PCLK148.5 */
	fpd_dp_ser_write_reg(client, 0xb4, 0x03);
	/* Program N value lower byte */
	fpd_dp_ser_write_reg(client, 0xb5, 0xc0);
	/* Program N value middle byte */
	fpd_dp_ser_write_reg(client, 0xb6, 0x7a);
	/* Program N value upper byte */
	fpd_dp_ser_write_reg(client, 0xb7, 0x10);
	/* Select Port 0 registers */
	fpd_dp_ser_write_reg(client, 0xe, 0x1);
}

/**
 * @brief Setup DTG for port 0
 * 
 * @param client 
 */
void fpd_dp_deser_setup_dtg(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Setup DTG for port 0\n");
	/* Select DTG Page */
	fpd_dp_ser_write_reg(client, 0x40, 0x50);
	fpd_dp_ser_write_reg(client, 0x41, 0x20);
	/* Set up Local Display DTG BPP, Sync Polarities, and Measurement Type */
	fpd_dp_ser_write_reg(client, 0x42, 0x9b);
	/* Set Hstart */
	fpd_dp_ser_write_reg(client, 0x41, 0x29);
	/* Hstart upper byte */
	fpd_dp_ser_write_reg(client, 0x42, 0x80);
	fpd_dp_ser_write_reg(client, 0x41, 0x2a);
	/* Hstart lower byte */
	fpd_dp_ser_write_reg(client, 0x42, 0x70);
	/* Set HSW */
	fpd_dp_ser_write_reg(client, 0x41, 0x2f);
	/* HSW upper byte */
	fpd_dp_ser_write_reg(client, 0x42, 0x40);
	fpd_dp_ser_write_reg(client, 0x41, 0x30);
	/* HSW lower byte */
	fpd_dp_ser_write_reg(client, 0x42, 0x20);
}

/**
 * @brief Program DPTX for DP port 0
 * 
 * @param client 
 */
void fpd_dp_deser_setup_dptx(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Program DPTX for DP port 0\n");
	/* Enable APB interface */
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set bit per color */
	fpd_dp_ser_write_reg(client, 0x49, 0xa4);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x20);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set pixel width */
	fpd_dp_ser_write_reg(client, 0x49, 0xb8);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x4);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set DP Mvid */
	fpd_dp_ser_write_reg(client, 0x49, 0xac);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x7d);
	fpd_dp_ser_write_reg(client, 0x4c, 0x72);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set DP Nvid */
	fpd_dp_ser_write_reg(client, 0x49, 0xb4);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x0);
	fpd_dp_ser_write_reg(client, 0x4c, 0x80);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set TU Mode */
	fpd_dp_ser_write_reg(client, 0x49, 0xc8);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x0);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set TU Size */
	fpd_dp_ser_write_reg(client, 0x49, 0xb0);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x40);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x2b);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set FIFO Size */
	fpd_dp_ser_write_reg(client, 0x49, 0xc8);
	fpd_dp_ser_write_reg(client, 0x4a, 0x0);
	fpd_dp_ser_write_reg(client, 0x4b, 0x6);
	fpd_dp_ser_write_reg(client, 0x4c, 0x40);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set data count */
	fpd_dp_ser_write_reg(client, 0x49, 0xbc);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x80);
	fpd_dp_ser_write_reg(client, 0x4c, 0x7);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Disable STREAM INTERLACED */
	fpd_dp_ser_write_reg(client, 0x49, 0xc0);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x0);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);

	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set SYNC polarity */
	fpd_dp_ser_write_reg(client, 0x49, 0xc4);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0xe);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
}

/**
 * @brief Release Des DTG reset
 * 
 * @param client 
 */
void fpd_dp_deser_release_dtg_reset(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Release Des 0 DTG reset and enable video output\n");
	/* Select DTG Page */
	fpd_dp_ser_write_reg(client, 0x40, 0x50);
	fpd_dp_ser_write_reg(client, 0x41, 0x32);
	/* Select DTG Page */
	fpd_dp_ser_write_reg(client, 0x42, 0x4);
	fpd_dp_ser_write_reg(client, 0x41, 0x62);
	/* Release Local Display Output Port 1 DTG */
	fpd_dp_ser_write_reg(client, 0x42, 0x4);

	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set Htotal */
	fpd_dp_ser_write_reg(client, 0x49, 0x80);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0xa0);
	fpd_dp_ser_write_reg(client, 0x4c, 0xa);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
}

/**
 * @brief Enable DP 0 output
 * 
 * @param client 
 */
void fpd_dp_deser_enable_output(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Enable DP 0 output\n");
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Enable DP output */
	fpd_dp_ser_write_reg(client, 0x49, 0x84);
	fpd_dp_ser_write_reg(client, 0x4a, 0x0);
	fpd_dp_ser_write_reg(client, 0x4b, 0x1);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
}

int fpd_dp_ser_prepare(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] %s:\n", __func__);
	fpd_dp_ser_reset(client);
	fpd_dp_ser_set_up_variables(client);
	fpd_dp_ser_check_mode_strapping(client);

	fpd_dp_priv->FPDConf = 2;

	return 0;
}

bool fpd_dp_ser_setup(struct i2c_client *client)
{
	fpd_dp_ser_program_fpd_4_mode(client);
	fpd_dp_set_fpd_4_pll(client);
	fpd_dp_ser_configue_enable_plls(client);
	fpd_dp_ser_enable_I2C_passthrough(client);
	fpd_dp_deser_soft_reset(client);
	fpd_dp_ser_set_dp_config(client);
	fpd_dp_ser_program_vp_configs(client);
	fpd_dp_ser_enable_vps(client);
	/* Check if VP is synchronized to DP input */
	//queue_delayed_work(fpd_dp_priv->wq, &fpd_dp_priv->delay_work, msecs_to_jiffies(100));

	return true;
}

bool fpd_dp_ser_enable(void)
{
	fpd_dp_ser_prepare(fpd_dp_priv->priv_dp_client[0]);
	if (false == fpd_dp_ser_setup(fpd_dp_priv->priv_dp_client[0])) {
		fpd_dp_ser_debug("[FPD_DP] DS90UB983 enable fail\n");
		return false;
	}
	return true;
}

bool fpd_dp_ser_disable(void)
{
	fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);
	return true;
}

/**
 * @brief Override DES 0 eFuse
 * @param client
 */
int fpd_dp_deser_984_override_efuse(struct i2c_client *client)
{
	u8 DES_READBACK;
	u8 UNIQUEID_Reg0xC;

	/* Override DES 0 eFuse */
	fpd_dp_ser_read_reg(client, 0x0, &DES_READBACK);

	if (DES_READBACK == 0)
		fpd_dp_ser_debug("[FPD_DP] Error - no DES detected\n");
	else
		fpd_dp_ser_debug("[FPD_DP] Deserializer detected successfully\n");
	/* i2c 400k */
	fpd_dp_ser_write_reg(client, 0x2b, 0x0a);
	fpd_dp_ser_write_reg(client, 0x2c, 0x0b);

	fpd_dp_ser_write_reg(client, 0x49, 0xc);
	fpd_dp_ser_write_reg(client, 0x4a, 0x0);
	fpd_dp_ser_write_reg(client, 0x48, 0x1b);
	fpd_dp_ser_read_reg(client, 0x4b, &UNIQUEID_Reg0xC);

	if (UNIQUEID_Reg0xC != 0x19 || DES_READBACK != 0) {
		fpd_dp_ser_debug("[FPD_DP] Non-Final DES Silicon Detected - Overriding DES eFuse");
		fpd_dp_ser_write_reg(client, 0xe, 0x3);
		fpd_dp_ser_write_reg(client, 0x61, 0x0);
		fpd_dp_ser_write_reg(client, 0x5a, 0x74);
		fpd_dp_ser_write_reg(client, 0x5f, 0x4);
		fpd_dp_ser_write_reg(client, 0x40, 0x3c);
		fpd_dp_ser_write_reg(client, 0x41, 0xf5);
		fpd_dp_ser_write_reg(client, 0x42, 0x21);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0x43);
		fpd_dp_ser_write_reg(client, 0x42, 0x3);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0x43);
		fpd_dp_ser_write_reg(client, 0x42, 0x3);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0x5);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0x5);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0x6);
		fpd_dp_ser_write_reg(client, 0x42, 0x1);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0x6);
		fpd_dp_ser_write_reg(client, 0x42, 0x1);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0x37);
		fpd_dp_ser_write_reg(client, 0x42, 0x32);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0x37);
		fpd_dp_ser_write_reg(client, 0x42, 0x32);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0x8d);
		fpd_dp_ser_write_reg(client, 0x42, 0xff);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0x8d);
		fpd_dp_ser_write_reg(client, 0x42, 0xff);
		fpd_dp_ser_write_reg(client, 0x40, 0x5c);
		fpd_dp_ser_write_reg(client, 0x41, 0x20);
		fpd_dp_ser_write_reg(client, 0x42, 0x3c);
		fpd_dp_ser_write_reg(client, 0x41, 0xa0);
		fpd_dp_ser_write_reg(client, 0x42, 0x3c);
		fpd_dp_ser_write_reg(client, 0x40, 0x38);
		fpd_dp_ser_write_reg(client, 0x41, 0x24);
		fpd_dp_ser_write_reg(client, 0x42, 0x61);
		fpd_dp_ser_write_reg(client, 0x41, 0x54);
		fpd_dp_ser_write_reg(client, 0x42, 0x61);
		fpd_dp_ser_write_reg(client, 0x41, 0x2c);
		fpd_dp_ser_write_reg(client, 0x42, 0x19);
		fpd_dp_ser_write_reg(client, 0x41, 0x5c);
		fpd_dp_ser_write_reg(client, 0x42, 0x19);
		fpd_dp_ser_write_reg(client, 0x41, 0x2e);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x41, 0x5e);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x40, 0x10);
		fpd_dp_ser_write_reg(client, 0x41, 0x18);
		fpd_dp_ser_write_reg(client, 0x42, 0x4b);
		fpd_dp_ser_write_reg(client, 0x41, 0x38);
		fpd_dp_ser_write_reg(client, 0x42, 0x4b);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0x15);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0x15);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0x4a);
		fpd_dp_ser_write_reg(client, 0x42, 0x1);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0x4a);
		fpd_dp_ser_write_reg(client, 0x42, 0x1);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0xaa);
		fpd_dp_ser_write_reg(client, 0x42, 0x2c);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0xaa);
		fpd_dp_ser_write_reg(client, 0x42, 0x2c);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0xab);
		fpd_dp_ser_write_reg(client, 0x42, 0x2c);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0xab);
		fpd_dp_ser_write_reg(client, 0x42, 0x2c);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0xac);
		fpd_dp_ser_write_reg(client, 0x42, 0x4c);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0xac);
		fpd_dp_ser_write_reg(client, 0x42, 0x4c);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0xad);
		fpd_dp_ser_write_reg(client, 0x42, 0x4c);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0xad);
		fpd_dp_ser_write_reg(client, 0x42, 0x4c);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0xae);
		fpd_dp_ser_write_reg(client, 0x42, 0xac);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0xae);
		fpd_dp_ser_write_reg(client, 0x42, 0xac);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0xaf);
		fpd_dp_ser_write_reg(client, 0x42, 0xac);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0xaf);
		fpd_dp_ser_write_reg(client, 0x42, 0xac);
		fpd_dp_ser_write_reg(client, 0x40, 0x10);
		fpd_dp_ser_write_reg(client, 0x41, 0x5);
		fpd_dp_ser_write_reg(client, 0x42, 0xa);
		fpd_dp_ser_write_reg(client, 0x41, 0x25);
		fpd_dp_ser_write_reg(client, 0x42, 0xa);
		fpd_dp_ser_write_reg(client, 0x40, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0x89);
		fpd_dp_ser_write_reg(client, 0x42, 0x38);
		fpd_dp_ser_write_reg(client, 0x40, 0x58);
		fpd_dp_ser_write_reg(client, 0x41, 0x89);
		fpd_dp_ser_write_reg(client, 0x42, 0x38);
		fpd_dp_ser_write_reg(client, 0x40, 0x10);
		fpd_dp_ser_write_reg(client, 0x41, 0x1a);
		fpd_dp_ser_write_reg(client, 0x42, 0x8);
		fpd_dp_ser_write_reg(client, 0x41, 0x3a);
		fpd_dp_ser_write_reg(client, 0x42, 0x8);
		fpd_dp_ser_write_reg(client, 0x40, 0x38);
		fpd_dp_ser_write_reg(client, 0x41, 0x6f);
		fpd_dp_ser_write_reg(client, 0x42, 0x54);
		fpd_dp_ser_write_reg(client, 0x41, 0x70);
		fpd_dp_ser_write_reg(client, 0x42, 0x5);
		fpd_dp_ser_write_reg(client, 0x41, 0x80);
		fpd_dp_ser_write_reg(client, 0x42, 0x55);
		fpd_dp_ser_write_reg(client, 0x41, 0x81);
		fpd_dp_ser_write_reg(client, 0x42, 0x44);
		fpd_dp_ser_write_reg(client, 0x41, 0x82);
		fpd_dp_ser_write_reg(client, 0x42, 0x3);
		fpd_dp_ser_write_reg(client, 0x41, 0x86);
		fpd_dp_ser_write_reg(client, 0x42, 0x2c);
		fpd_dp_ser_write_reg(client, 0x41, 0x87);
		fpd_dp_ser_write_reg(client, 0x42, 0x6);
		fpd_dp_ser_write_reg(client, 0x41, 0x18);
		fpd_dp_ser_write_reg(client, 0x42, 0x32);
		fpd_dp_ser_write_reg(client, 0x41, 0x48);
		fpd_dp_ser_write_reg(client, 0x42, 0x32);
		fpd_dp_ser_write_reg(client, 0x41, 0x19);
		fpd_dp_ser_write_reg(client, 0x42, 0xe);
		fpd_dp_ser_write_reg(client, 0x41, 0x49);
		fpd_dp_ser_write_reg(client, 0x42, 0xe);
		fpd_dp_ser_write_reg(client, 0x41, 0x17);
		fpd_dp_ser_write_reg(client, 0x42, 0x72);
		fpd_dp_ser_write_reg(client, 0x41, 0x47);
		fpd_dp_ser_write_reg(client, 0x42, 0x72);
		fpd_dp_ser_write_reg(client, 0x41, 0x26);
		fpd_dp_ser_write_reg(client, 0x42, 0x87);
		fpd_dp_ser_write_reg(client, 0x41, 0x56);
		fpd_dp_ser_write_reg(client, 0x42, 0x87);
		fpd_dp_ser_write_reg(client, 0x40, 0x2c);
		fpd_dp_ser_write_reg(client, 0x41, 0x3d);
		fpd_dp_ser_write_reg(client, 0x42, 0xd5);
		fpd_dp_ser_write_reg(client, 0x41, 0x3e);
		fpd_dp_ser_write_reg(client, 0x42, 0x15);
		fpd_dp_ser_write_reg(client, 0x41, 0x7d);
		fpd_dp_ser_write_reg(client, 0x42, 0xd5);
		fpd_dp_ser_write_reg(client, 0x41, 0x7e);
		fpd_dp_ser_write_reg(client, 0x42, 0x15);
		fpd_dp_ser_write_reg(client, 0x41, 0x82);
		fpd_dp_ser_write_reg(client, 0x42, 0x1);
		fpd_dp_ser_write_reg(client, 0x41, 0x29);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x40, 0x10);
		fpd_dp_ser_write_reg(client, 0x41, 0x41);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x41, 0x42);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x40, 0x24);
		fpd_dp_ser_write_reg(client, 0x41, 0x20);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x41, 0x21);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x41, 0x23);
		fpd_dp_ser_write_reg(client, 0x42, 0x30);
		fpd_dp_ser_write_reg(client, 0x40, 0x10);
		fpd_dp_ser_write_reg(client, 0x41, 0x14);
		fpd_dp_ser_write_reg(client, 0x42, 0x78);
		fpd_dp_ser_write_reg(client, 0x41, 0x35);
		fpd_dp_ser_write_reg(client, 0x42, 0x7e);
		fpd_dp_ser_write_reg(client, 0x40, 0x6c);
		fpd_dp_ser_write_reg(client, 0x41, 0xd);
		fpd_dp_ser_write_reg(client, 0x42, 0x0);
		fpd_dp_ser_write_reg(client, 0x40, 0x1c);
		fpd_dp_ser_write_reg(client, 0x41, 0x8);
		fpd_dp_ser_write_reg(client, 0x42, 0x13);
		fpd_dp_ser_write_reg(client, 0x41, 0x28);
		fpd_dp_ser_write_reg(client, 0x42, 0x13);
		fpd_dp_ser_write_reg(client, 0x40, 0x14);
		fpd_dp_ser_write_reg(client, 0x41, 0x62);
		fpd_dp_ser_write_reg(client, 0x42, 0x31);
		fpd_dp_ser_write_reg(client, 0x41, 0x72);
		fpd_dp_ser_write_reg(client, 0x42, 0x31);
		fpd_dp_ser_write_reg(client, 0x41, 0x61);
		fpd_dp_ser_write_reg(client, 0x42, 0x26);
		fpd_dp_ser_write_reg(client, 0x41, 0x71);
		fpd_dp_ser_write_reg(client, 0x42, 0x26);
		/* Soft Reset DES */
		fpd_dp_ser_write_reg(client, 0x1, 0x1);
		usleep_range(30000, 32000);
	}

	return 0;
}

/**
 * @brief Program quad pixel clock for DP port 0
 * @param client
 */
void fpd_dp_deser_984_prog_pclk(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Program quad pixel clock for DP port 0\n");
	/* Select Port0 registers */
	fpd_dp_ser_write_reg(client, 0xe, 0x1);
	/* Enable clock divider */
	fpd_dp_ser_write_reg(client, 0xb1, 0x1);
	/* Program M value lower byte */
	fpd_dp_ser_write_reg(client, 0xb2, 0xb6);
	/* Program M value middle byte */
	fpd_dp_ser_write_reg(client, 0xb3, 0x30);
	/* Program M value upper byte*/
	fpd_dp_ser_write_reg(client, 0xb4, 0x5);
	/* Program N value lower byte */
	fpd_dp_ser_write_reg(client, 0xb5, 0xc0);
	/* Program N value middle byte */
	fpd_dp_ser_write_reg(client, 0xb6, 0x7a);
	/* Program N value upper byte */
	fpd_dp_ser_write_reg(client, 0xb7, 0x10);
	/* Select Port 0 registers */
	fpd_dp_ser_write_reg(client, 0xe, 0x1);
}

/**
 * @brief Setup DTG for port 0
 * @param client
 */
void fpd_dp_deser_984_setup_dtg(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Setup DTG for port 0\n");
	/* Select DTG Page */
	fpd_dp_ser_write_reg(client, 0x40, 0x50);
	fpd_dp_ser_write_reg(client, 0x41, 0x20);
	/* Set up Local Display DTG BPP, Sync Polarities, and Measurement Type */
	fpd_dp_ser_write_reg(client, 0x42, 0x93);
	/* Set Hstart */
	fpd_dp_ser_write_reg(client, 0x41, 0x29);
	/* Hstart upper byte */
	fpd_dp_ser_write_reg(client, 0x42, 0x82);
	fpd_dp_ser_write_reg(client, 0x41, 0x2a);
	/* Hstart lower byte */
	fpd_dp_ser_write_reg(client, 0x42, 0x0);
	/* Set HSW */
	fpd_dp_ser_write_reg(client, 0x41, 0x2f);
	/* HSW upper byte */
	fpd_dp_ser_write_reg(client, 0x42, 0x40);
	fpd_dp_ser_write_reg(client, 0x41, 0x30);
	/* HSW lower byte */
	fpd_dp_ser_write_reg(client, 0x42, 0x20);
}


/**
 * @brief Program DPTX for DP port 0
 * @param client
 */
void fpd_dp_deser_984_setup_dptx(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Program DPTX for DP port 0\n");
	/* Enable APB interface */
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set bit per color */
	fpd_dp_ser_write_reg(client, 0x49, 0xa4);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x20);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set pixel width */
	fpd_dp_ser_write_reg(client, 0x49, 0xb8);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x4);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set DP Mvid */
	fpd_dp_ser_write_reg(client, 0x49, 0xac);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x41);
	fpd_dp_ser_write_reg(client, 0x4c, 0xa1);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set DP Nvid */
	fpd_dp_ser_write_reg(client, 0x49, 0xb4);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x0);
	fpd_dp_ser_write_reg(client, 0x4c, 0x80);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set TU Mode */
	fpd_dp_ser_write_reg(client, 0x49, 0xc8);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x0);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set TU Size */
	fpd_dp_ser_write_reg(client, 0x49, 0xb0);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x40);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x3c);
	fpd_dp_ser_write_reg(client, 0x4e, 0x8);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set FIFO Size */
	fpd_dp_ser_write_reg(client, 0x49, 0xc8);
	fpd_dp_ser_write_reg(client, 0x4a, 0x0);
	fpd_dp_ser_write_reg(client, 0x4b, 0x4);
	fpd_dp_ser_write_reg(client, 0x4c, 0x40);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set data count */
	fpd_dp_ser_write_reg(client, 0x49, 0xbc);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x70);
	fpd_dp_ser_write_reg(client, 0x4c, 0x8);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Disable STREAM INTERLACED */
	fpd_dp_ser_write_reg(client, 0x49, 0xc0);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x0);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set SYNC polarity */
	fpd_dp_ser_write_reg(client, 0x49, 0xc4);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0xc);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
}

/**
 * @brief Release Des DTG reset
 * @param client
 */
void fpd_dp_deser_984_release_dtg_reset(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Release Des 0 DTG reset and enable video output\n");
	/* Select DTG Page */
	fpd_dp_ser_write_reg(client, 0x40, 0x50);
	fpd_dp_ser_write_reg(client, 0x41, 0x32);
	/* Release Local Display Output Port 0 DTG */
	fpd_dp_ser_write_reg(client, 0x42, 0x4);
	fpd_dp_ser_write_reg(client, 0x41, 0x62);
	/* Release Local Display Output Port 1 DTG */
	fpd_dp_ser_write_reg(client, 0x42, 0x4);
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Set Htotal */
	fpd_dp_ser_write_reg(client, 0x49, 0x80);
	fpd_dp_ser_write_reg(client, 0x4a, 0x1);
	fpd_dp_ser_write_reg(client, 0x4b, 0x70);
	fpd_dp_ser_write_reg(client, 0x4c, 0xd);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
}

/**
 * @brief Enable DP 0 output
 * @param client
 */
void fpd_dp_deser_984_enable_output(struct i2c_client *client)
{
	fpd_dp_ser_debug("[FPD_DP] Enable DP 0 output\n");
	fpd_dp_ser_write_reg(client, 0x48, 0x1);
	/* Enable DP output */
	fpd_dp_ser_write_reg(client, 0x49, 0x84);
	fpd_dp_ser_write_reg(client, 0x4a, 0x0);
	fpd_dp_ser_write_reg(client, 0x4b, 0x1);
	fpd_dp_ser_write_reg(client, 0x4c, 0x0);
	fpd_dp_ser_write_reg(client, 0x4d, 0x0);
	fpd_dp_ser_write_reg(client, 0x4e, 0x0);
	/* Enable INTB_IN */
	fpd_dp_ser_write_reg(client, 0x44, 0x81);
}

void fpd_dp_deser_984_enable(void)
{
	fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);

	fpd_dp_ser_configure_serializer_tx_link_layer(fpd_dp_priv->priv_dp_client[0]);
	fpd_dp_deser_984_override_efuse(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_ser_clear_crc_error(fpd_dp_priv->priv_dp_client[0]);
	fpd_dp_deser_hold_dtg_reset(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_disalbe_stream_mapping(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_force_dp_rate(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_setup_ports(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_map_output(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_984_prog_pclk(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_984_setup_dtg(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_984_setup_dptx(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_984_release_dtg_reset(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_984_enable_output(fpd_dp_priv->priv_dp_client[1]);
}

/**
 * @brief Check if VP is synchronized to DP input
 * @param work
 */
static void fpd_poll_984_training(void)
{
	u8 VP0sts = 0;

	fpd_dp_ser_debug("[FPD_DP] Check if VP is synchronized to DP input\n");

	/* Delay for VPs to sync to DP source */
	usleep_range(20000, 22000);

	/* Select VP Page */
	fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x40, 0x31);
	fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x41, 0x30);
	fpd_dp_ser_read_reg(fpd_dp_priv->priv_dp_client[0], 0x42, &VP0sts);
	fpd_dp_ser_debug("[FPD_DP] VP0sts = 0x%02x\n", (VP0sts & 0x01));

	if (((VP0sts & 0x01) == 0)) {
		fpd_dp_ser_debug("[FPD_DP]  VPs not synchronized - performing video input reset\n");
		/* Video Input Reset if VP is not synchronized */
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x49, 0x54);
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x4a, 0x0);
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x4b, 0x1);
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x4c, 0x0);
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x4d, 0x0);
		fpd_dp_ser_write_reg(fpd_dp_priv->priv_dp_client[0], 0x4e, 0x0);
	}

	fpd_dp_ser_debug("[FPD_DP] ser training lock completed, count = %d\n", fpd_dp_priv->count);

	/* Delay for VPs to sync to DP source */
	usleep_range(20000, 22000);

	fpd_dp_deser_984_enable();
}

void fpd_dp_deser_enable(void)
{
	fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);
	fpd_dp_ser_configure_serializer_tx_link_layer(fpd_dp_priv->priv_dp_client[0]);
	fpd_dp_deser_override_efuse(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_set_up_des_temp(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_ser_clear_crc_error(fpd_dp_priv->priv_dp_client[0]);
	fpd_dp_deser_hold_dtg_reset(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_disalbe_stream_mapping(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_force_dp_rate(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_setup_ports(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_map_output(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_prog_pclk(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_setup_dtg(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_setup_dptx(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_release_dtg_reset(fpd_dp_priv->priv_dp_client[1]);
	fpd_dp_deser_enable_output(fpd_dp_priv->priv_dp_client[1]);
}

void fpd_dp_deser_disable(void)
{
	fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);
}

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

	while (retry_count < 100) {
		i = 0;
		found = 0;
		while ((adapter = i2c_get_adapter(i)) != NULL) {
			parent = adapter->dev.parent;
			pp = parent->parent;
			i2c_put_adapter(adapter);
			fpd_dp_ser_debug("[FPD_DP] dev_name(pp): %s\n", dev_name(pp));
			if (pp && !strncmp(adapter_bdf, dev_name(pp), bdf_len)) {
				found = 1;
				break;
			}
			i++;
		}

		if (found) {
			fpd_dp_ser_debug("[FPD_DP] found dev_name(pp) %s\n", dev_name(pp));
			break;
		}
		retry_count++;
		fpd_dp_ser_debug("[FPD_DP] not found retry_count %d\n", retry_count);
		msleep(50);
	}

	if (found)
		return i;

	/* Not found */
	return -1;
}

int fpd_dp_ser_get_i2c_bus_number(void)
{
	char adapter_bdf[32] = ADAPTER_PP_DEV_NAME;
	int bus_number = intel_get_i2c_bus_id(0, adapter_bdf, 32);
	return bus_number;
}
EXPORT_SYMBOL_GPL(fpd_dp_ser_get_i2c_bus_number);

bool fpd_dp_ser_init(void)
{
	fpd_dp_ser_lock_global();

	fpd_dp_ser_enable();

	/* Check if VP is synchronized to DP input */
	fpd_poll_984_training();

	WRITE_ONCE(deser_reset, 0);

	fpd_dp_ser_set_up_mcu(fpd_dp_priv->priv_dp_client[0]);

	fpd_dp_ser_set_ready(true);

	fpd_dp_ser_unlock_global();

	if (!fpd_dp_priv->priv_dp_client[2])
		fpd_dp_priv->priv_dp_client[2] = i2c_new_dummy_device(fpd_dp_priv->i2c_adap, fpd_dp_i2c_board_info[2].addr);

	fpd_dp_ser_lock_global();
	fpd_dp_ser_motor_open(fpd_dp_priv->priv_dp_client[2]);
	fpd_dp_ser_unlock_global();

	return true;
}

static int fpd_dp_ser_probe(struct platform_device *pdev)
{
	struct i2c_adapter *i2c_adap;
	struct fpd_dp_ser_priv *priv;
	int bus_number = 0;
	int ret = 0;
	unsigned char  __iomem *gpio_cfg;
	unsigned char data;

	priv = devm_kzalloc(&pdev->dev, sizeof(struct fpd_dp_ser_priv),
			GFP_KERNEL);

	if (priv == NULL) {
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, priv);

	memset(priv, 0, sizeof(*priv));
	priv->dev = &pdev->dev;

	bus_number = fpd_dp_ser_get_i2c_bus_number();
	fpd_dp_ser_debug("Use bus_number %d \n", bus_number);
	i2c_adap = i2c_get_adapter(bus_number);
	if (!i2c_adap) {
		fpd_dp_ser_debug("Cannot find a valid i2c bus for max serdes\n");
		return -ENOMEM;
	}

	/* retries when i2c timeout */
	i2c_adap->retries = 5;
	i2c_adap->timeout = msecs_to_jiffies(5 * 10);
	i2c_put_adapter(i2c_adap);
	priv->i2c_adap = i2c_adap;

	i2c_adap_mcu = i2c_adap;

	priv->priv_dp_client[0] = i2c_new_dummy_device(i2c_adap, fpd_dp_i2c_board_info[0].addr);
	i2c_set_clientdata(priv->priv_dp_client[0], priv);

	fpd_dp_priv = priv;

	fpd_dp_priv->wq = alloc_workqueue("fpd_poll_training_lock",
			WQ_HIGHPRI, 0);

	if (unlikely(!fpd_dp_priv->wq)) {
		fpd_dp_ser_debug("[FPD_DP] Failed to allocate workqueue\n");
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&fpd_dp_priv->delay_work,
			fpd_poll_training_lock);

	fpd_dp_priv->count = 0;

	gpio_cfg = (unsigned char *)ioremap(PAD_CFG_DW0_GPPC_A_16, 0x1);
	data = ioread8(gpio_cfg);
	data = data | 1;
	iowrite8(data, gpio_cfg);
	iounmap(gpio_cfg);
	/* Delay for VPs to sync to DP source */
	usleep_range(5000, 5200);

	fpd_dp_ser_init();

	return ret;
}

static int fpd_dp_ser_remove(struct platform_device *pdev) {
	int i = 0;
	struct fpd_dp_ser_priv *priv =
		(struct fpd_dp_ser_priv*)platform_get_drvdata(pdev);
	fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);
	if (priv != NULL) {
		cancel_delayed_work_sync(&priv->delay_work);
		fpd_dp_ser_lock_global();
		fpd_dp_ser_set_ready(false);
		for (i = 0; i < NUM_DEVICE; i++) {
			struct i2c_client *client= priv->priv_dp_client[i];
			if (i == 0)
				fpd_dp_ser_reset(client);
			else
				fpd_dp_ser_write_reg(client, 0x01, 0x01);
			if (client != NULL) {
				if (i == 2)
					fpd_dp_ser_motor_close(client);

				i2c_unregister_device(client);
			}
		}
		fpd_dp_ser_unlock_global();
		devm_kfree(&pdev->dev, priv);
		fpd_dp_ser_debug("[-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);
	}
	return 0;
}

#ifdef CONFIG_PM

static int fpd_dp_ser_suspend(struct device *dev)
{
#if 1
	int i = 0;
	struct fpd_dp_ser_priv *priv = dev_get_drvdata(dev);

	fpd_dp_ser_lock_global();
	fpd_dp_ser_set_ready(false);
	/* first des reset, and then ser reset */
	for (i = 1; i > -1; i--) {
		struct i2c_client *client= priv->priv_dp_client[i];
		if (i == 0)
			fpd_dp_ser_reset(client);
		else
			fpd_dp_ser_write_reg(client, 0x01, 0x01);

		/* after reset, wait 20ms to avoid ser/des read/write fail */
		usleep_range(20000, 22000);
	}
	fpd_dp_ser_unlock_global();
#endif
	fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);
	return 0;	
}

static int fpd_dp_ser_resume(struct device *dev)
{
	bool result;

	fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);

	result = fpd_dp_ser_init();
	if (!result) {
		fpd_dp_ser_debug("Serdes enable fail in fpd_dp_ser_resume\n");
		return -EIO;
	}

	return 0;
}

static int fpd_dp_ser_runtime_suspend(struct device *dev)
{
        struct fpd_dp_ser_priv *priv = dev_get_drvdata(dev);
        fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);
        return 0;
}

static int fpd_dp_ser_runtime_resume(struct device *dev)
{
        struct fpd_dp_ser_priv *priv = dev_get_drvdata(dev);
        bool result;

        fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);
        result = fpd_dp_ser_init();
        return 0;
}

static int fpd_dp_ser_resume_early(struct device *dev)
{
	unsigned char  __iomem *gpio_cfg;
	unsigned char data;

	fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);

	/* Map  GPIO IO address to virtual address */
	gpio_cfg = (unsigned char *)ioremap(PAD_CFG_DW0_GPPC_A_16, 0x1);
	if (!gpio_cfg) {
		fpd_dp_ser_debug("Ioremap fail in fpd_dp_ser_resume_early\n");
		return -ENOMEM;
	}

	data = ioread8(gpio_cfg);

	/* Set Serdes DP3 last bit of GPIO IO address to 1 for pulling up DP3 */
	data = data | 1;
	iowrite8(data, gpio_cfg);
	iounmap(gpio_cfg);

	return 0;
}

static const struct dev_pm_ops fdp_dp_ser_pmops = {
	.suspend	= fpd_dp_ser_suspend,
	.resume		= fpd_dp_ser_resume,
	.runtime_suspend = fpd_dp_ser_runtime_suspend,
	.runtime_resume  = fpd_dp_ser_runtime_resume,
	.resume_early = fpd_dp_ser_resume_early,
};

#define FDP_DP_SER_PMOPS (&fdp_dp_ser_pmops)

#else

#define FDP_DP_SER_PMOPS NULL

#endif

static const struct i2c_device_id fpd_dp_ser_i2c_id_table[] = {
	{ "DS90UB983",  DS90UB983 },
	{ "DS90UB984",  DS90UB984 },
	{ "DS90UBMCU",  DS90UBMCU },
	{ },
};

#define DEV_NAME "fpd_dp_ser_drv"

static struct platform_driver fpd_dp_ser_driver = {
	.probe	= fpd_dp_ser_probe,
	.remove	= fpd_dp_ser_remove,
	.driver		= {
		.name  = DEV_NAME,
		.owner = THIS_MODULE,
		.pm = FDP_DP_SER_PMOPS,
	},
};

int __init fpd_dp_ser_module_init(void)
{
	int ret = 0;

	pdev = platform_device_register_simple(DEV_NAME, -1, NULL, 0);
	fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);

	if (!IS_ERR(pdev)) {
		ret = platform_driver_probe(&fpd_dp_ser_driver,
				fpd_dp_ser_probe);
		if (ret) {
			pr_err("Can't probe platform driver\n");
			platform_device_unregister(pdev);
		}
	} else
		ret = PTR_ERR(pdev);

	return ret;
}

void __exit fpd_dp_ser_module_exit(void)
{
	fpd_dp_ser_debug("[FPD_DP] [-%s-%s-%d-]\n", __FILE__, __func__, __LINE__);
	platform_device_unregister(pdev);
	platform_driver_unregister(&fpd_dp_ser_driver);
}

EXPORT_SYMBOL(i2c_adap_mcu);
EXPORT_SYMBOL(deser_reset);

#ifdef MODULE
module_init(fpd_dp_ser_module_init);
module_exit(fpd_dp_ser_module_exit);
#else
late_initcall(fpd_dp_ser_module_init);
#endif

MODULE_DESCRIPTION("TI serdes 983 984 driver");
MODULE_AUTHOR("Jia, Lin A <lin.a.jia@intel.com>");
MODULE_AUTHOR("Hu, Kanli <kanli.hu@intel.com>");
MODULE_LICENSE("GPL v2");
