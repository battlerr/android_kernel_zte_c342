/******************** (C) COPYRIGHT 2010 STMicroelectronics ********************
 *
 * File Name          : KXTJ1004_acc.c
 * Authors            : MSH - Motion Mems BU - Application Team
 *		      : Carmine Iascone (carmine.iascone@st.com)
 *		      : Matteo Dameno (matteo.dameno@st.com)
 * Version            : V.1.0.5
 * Date               : 16/08/2010
 * Description        : KXTJ1004 accelerometer sensor API
 *
 *******************************************************************************
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THE PRESENT SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES
 * OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, FOR THE SOLE
 * PURPOSE TO SUPPORT YOUR APPLICATION DEVELOPMENT.
 * AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY DIRECT,
 * INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING FROM THE
 * CONTENT OF SUCH SOFTWARE AND/OR THE USE MADE BY CUSTOMERS OF THE CODING
 * INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
 *
 * THIS SOFTWARE IS SPECIFICALLY DESIGNED FOR EXCLUSIVE USE WITH ST PARTS.
 *
 ******************************************************************************
 Revision 1.0.0 05/11/09
 First Release
 Revision 1.0.3 22/01/2010
  Linux K&R Compliant Release;
 Revision 1.0.5 16/08/2010
 modified _get_acceleration_data function
 modified _update_odr function
 manages 2 interrupts

 ******************************************************************************/

#include	<linux/module.h>
#include	<linux/err.h>
#include	<linux/errno.h>
#include	<linux/delay.h>
#include	<linux/fs.h>
#include	<linux/i2c.h>

#include	<linux/input.h>
#include	<linux/input-polldev.h>
#include	<linux/miscdevice.h>
#include	<linux/uaccess.h>
#include        <linux/slab.h>

#include	<linux/workqueue.h>
#include	<linux/irq.h>
#include	<linux/gpio.h>
#include	<linux/interrupt.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include        <linux/earlysuspend.h>
#endif

#include "kxtj1004.h"
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>

#define	INTERRUPT_MANAGEMENT 1

#define	G_MAX		16000	/** Maximum polled-device-reported g value */

/*
#define	SHIFT_ADJ_2G		4
#define	SHIFT_ADJ_4G		3
#define	SHIFT_ADJ_8G		2
#define	SHIFT_ADJ_16G		1
*/

#define SENSITIVITY_2G		1	/**	mg/LSB	*/
#define SENSITIVITY_4G		2	/**	mg/LSB	*/
#define SENSITIVITY_8G		4	/**	mg/LSB	*/
#define SENSITIVITY_16G		12	/**	mg/LSB	*/

#define	HIGH_RESOLUTION		0x08

#define	AXISDATA_REG		0x28
#define WHOAMI_KXTJ1004_ACC	0x05	/*      Expctd content for WAI  */
#define WHOAMI2_KXTJ1004_ACC	0x08	
/*	CONTROL REGISTERS	*/
#define G_MAX			8000
/* OUTPUT REGISTERS */
#define XOUT_L			0x06
#define WHO_AM_I		0x0F	/*      WhoAmI register         */
#define INT_REL			0x1A
#define CTRL_REG1		0x1B
#define INT_CTRL1		0x1E
#define DATA_CTRL		0x21
/* CONTROL REGISTER 1 BITS */
#define PC1_OFF			0x7F
#define PC1_ON			(1 << 7)
/* Data ready funtion enable bit: set during probe if using irq mode */
#define DRDYE			(1 << 5)
/*	end CONTROL REGISTRES	*/



#define ODR12_5F		0
#define ODR25F			1
#define ODR50F			2
#define ODR100F		3
#define ODR200F		4
#define ODR400F		5
#define ODR800F		6
/* INTERRUPT CONTROL REGISTER 1 BITS */
/* Set these during probe if using irq mode */
#define KXTJ1004_IEL		(1 << 3)
#define KXTJ1004_IEA		(1 << 4)
#define KXTJ1004_IEN		(1 << 5)
/* RESUME STATE INDICES */
#define RES_DATA_CTRL		0
#define RES_CTRL_REG1		1
#define RES_INT_CTRL1		2
#define RESUME_ENTRIES		3

#define	RESUME_ENTRIES		20
#define DEVICE_INFO             "ST, LIS3DH"
#define DEVICE_INFO_LEN         32
/* end RESUME STATE INDICES */

//#define GSENSOR_GINT1_GPI 0
//#define GSENSOR_GINT2_GPI 1

#define GSENSOR_CALI_SUPPORT

#ifdef GSENSOR_CALI_SUPPORT
#define GSENSOR_CAL_FILE_PATH			"/productinfo/kxtj_cal"       //kxtj Calbration file path
#endif

#define	FUZZ			32
#define	FLAT			32
#define	I2C_RETRY_DELAY		5
#define	I2C_RETRIES		2
#define	I2C_AUTO_INCREMENT	0x80
struct {
	unsigned int cutoff_ms;
	unsigned int mask;
} KXTJ1004_acc_odr_table[] = {
	
	{ 3,	ODR800F },
	{ 5,	ODR400F },
	{ 10,	ODR200F },
	{ 20,	ODR100F },
	{ 40,	ODR50F  },
	{ 80,	ODR25F  },
	{ 0,	ODR12_5F},
};

struct KXTJ1004_acc_data {
	struct i2c_client *client;
	struct KXTJ1004_acc_platform_data *pdata;

	struct mutex lock;
	struct delayed_work input_work;

	struct input_dev *input_dev;

	int hw_initialized;
	/* hw_working=-1 means not tested yet */
	int hw_working;
	atomic_t enabled;
	int on_before_suspend;

	u8 sensitivity;
	u8 shift;
	u8 ctrl_reg1;
	u8 data_ctrl;
	u8 int_ctrl;
	u8 resume_state[RESUME_ENTRIES];

#ifdef GSENSOR_CALI_SUPPORT
	int get_cali_flag;
#endif

	int irq1;
	struct work_struct irq1_work;
	struct workqueue_struct *irq1_work_queue;
	int irq2;
	struct work_struct irq2_work;
	struct workqueue_struct *irq2_work_queue;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
};

#ifdef GSENSOR_CALI_SUPPORT
static int accel_cali[3] = {0,0,0};
#endif

/*
 * Because misc devices can not carry a pointer from driver register to
 * open, we keep this global.  This limits the driver to a single instance.
 */
struct KXTJ1004_acc_data *KXTJ1004_acc_misc_data;
struct i2c_client *KXTJ1004_i2c_client;
int KXTJ1004_acc_update_g_range(struct KXTJ1004_acc_data *acc, u8 new_g_range);
int KXTJ1004_acc_update_odr(struct KXTJ1004_acc_data *acc, int poll_interval_ms);
static int KXTJ1004_acc_i2c_read(struct KXTJ1004_acc_data *acc, u8 * buf, int len)
{
	int err;
	int tries = 0;

	struct i2c_msg msgs[] = {
		{
		 .addr = acc->client->addr,
		 .flags = acc->client->flags & I2C_M_TEN,
		 .len = 1,
		 .buf = buf,},
		{
		 .addr = acc->client->addr,
		 .flags = (acc->client->flags & I2C_M_TEN) | I2C_M_RD,
		 .len = len,
		 .buf = buf,},
	};

	do {
		err = i2c_transfer(acc->client->adapter, msgs, 2);
		if (err != 2)
			msleep_interruptible(I2C_RETRY_DELAY);
	} while ((err != 2) && (++tries < I2C_RETRIES));

	if (err != 2) {
		dev_err(&acc->client->dev, "read transfer error\n");
		err = -EIO;
	} else {
		err = 0;
	}

	return err;
}

static int KXTJ1004_acc_i2c_write(struct KXTJ1004_acc_data *acc, u8 * buf, int len)
{
	int err;
	int tries = 0;

	struct i2c_msg msgs[] = { {.addr = acc->client->addr,
				   .flags = acc->client->flags & I2C_M_TEN,
				   .len = len + 1,.buf = buf,},
	};
	do {
		err = i2c_transfer(acc->client->adapter, msgs, 1);
		if (err != 1)
			msleep_interruptible(I2C_RETRY_DELAY);
	} while ((err != 1) && (++tries < I2C_RETRIES));

	if (err != 1) {
		dev_err(&acc->client->dev, "write transfer error\n");
		err = -EIO;
	} else {
		err = 0;
	}

	return err;
}

static int KXTJ1004_acc_hw_init(struct KXTJ1004_acc_data *acc)
{
	int err = -1;
	u8 buf[7];

	pr_debug("%s: hw init start\n", KXTJ1004_ACC_DEV_NAME);


#if 0

	buf[0] = CTRL_REG1;
	buf[1] = acc->resume_state[RES_CTRL_REG1];
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		goto error1;

	buf[0] = TEMP_CFG_REG;
	buf[1] = acc->resume_state[RES_TEMP_CFG_REG];
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		goto error1;

	buf[0] = FIFO_CTRL_REG;
	buf[1] = acc->resume_state[RES_FIFO_CTRL_REG];
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		goto error1;

	buf[0] = (I2C_AUTO_INCREMENT | TT_THS);
	buf[1] = acc->resume_state[RES_TT_THS];
	buf[2] = acc->resume_state[RES_TT_LIM];
	buf[3] = acc->resume_state[RES_TT_TLAT];
	buf[4] = acc->resume_state[RES_TT_TW];
	err = KXTJ1004_acc_i2c_write(acc, buf, 4);
	if (err < 0)
		goto error1;
	buf[0] = TT_CFG;
	buf[1] = acc->resume_state[RES_TT_CFG];
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		goto error1;

	buf[0] = (I2C_AUTO_INCREMENT | INT_THS1);
	buf[1] = acc->resume_state[RES_INT_THS1];
	buf[2] = acc->resume_state[RES_INT_DUR1];
	err = KXTJ1004_acc_i2c_write(acc, buf, 2);
	if (err < 0)
		goto error1;
	buf[0] = INT_CFG1;
	buf[1] = acc->resume_state[RES_INT_CFG1];
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		goto error1;

	buf[0] = (I2C_AUTO_INCREMENT | INT_THS2);
	buf[1] = acc->resume_state[RES_INT_THS2];
	buf[2] = acc->resume_state[RES_INT_DUR2];
	err = KXTJ1004_acc_i2c_write(acc, buf, 2);
	if (err < 0)
		goto error1;
	buf[0] = INT_CFG2;
	buf[1] = acc->resume_state[RES_INT_CFG2];
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		goto error1;

	buf[0] = (I2C_AUTO_INCREMENT | CTRL_REG2);
	buf[1] = acc->resume_state[RES_CTRL_REG2];
	buf[2] = acc->resume_state[RES_CTRL_REG3];
	buf[3] = acc->resume_state[RES_CTRL_REG4];
	buf[4] = acc->resume_state[RES_CTRL_REG5];
	buf[5] = acc->resume_state[RES_CTRL_REG6];
	err = KXTJ1004_acc_i2c_write(acc, buf, 5);
	if (err < 0)
		goto error1;
#endif
/* ensure that PC1 is cleared before updating control registers */
  buf[0] = CTRL_REG1;
	buf[1] = 0;
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		goto error1;

#if 0
	/* only write INT_CTRL_REG1 if in irq mode */
	if (acc->client->irq) {
		err = i2c_smbus_write_byte_data(acc->client,
						INT_CTRL1, acc->int_ctrl);
		if (err < 0)
			return err;
	}
#endif


  err = KXTJ1004_acc_update_g_range(acc, acc->pdata->g_range);
	if (err < 0) {
//		dev_err(&client->dev, "update_g_range failed\n");
		goto error1;
	}





	/* turn on outputs */
	acc->ctrl_reg1 |= (PC1_ON | (1<<6));


  buf[0] = CTRL_REG1;
	buf[1] = acc->ctrl_reg1;
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		goto error1;



	err = KXTJ1004_acc_update_odr(acc, acc->pdata->poll_interval);
	if (err < 0) {
		//dev_err(&client->dev, "update_odr failed\n");
		goto error1;
	}


#if 0
	/* clear initial interrupt if in irq mode */
	if (tj9->client->irq) {
		err = i2c_smbus_read_byte_data(tj9->client, INT_REL);
		if (err < 0) {
			dev_err(&tj9->client->dev,
				"error clearing interrupt: %d\n", err);
			goto fail;
		}
	}
#endif
	return 0;
#if 0
	acc->hw_initialized = 1;
	pr_debug("%s: hw init done\n", KXTJ1004_ACC_DEV_NAME);
	return 0;
#endif

error_firstread:
	acc->hw_working = 0;
	dev_warn(&acc->client->dev, "Error reading WHO_AM_I: is device "
		 "available/working?\n");
	goto error1;
error_unknown_device:
	dev_err(&acc->client->dev,
		"device unknown. Expected: 0x%x,"
		" Replies: 0x%x\n", WHOAMI_KXTJ1004_ACC, buf[0]);
error1:
	acc->hw_initialized = 0;
	dev_err(&acc->client->dev, "hw init error 0x%x,0x%x: %d\n", buf[0],
		buf[1], err);
	return err;
}

static void KXTJ1004_acc_device_power_off(struct KXTJ1004_acc_data *acc)
{
	int err;
	u8 buf[2];
	#if 0
	u8 buf[2] = { CTRL_REG1, KXTJ1004_ACC_PM_OFF };

	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		dev_err(&acc->client->dev, "soft power off failed: %d\n", err);
#endif
  acc->ctrl_reg1 &= PC1_OFF;
	
  buf[0] = CTRL_REG1;
	buf[1] = acc->ctrl_reg1;
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		dev_err(&acc->client->dev, "soft power off failed\n");

	if (acc->pdata->power_off) {
		if (acc->irq1 != 0)
			disable_irq_nosync(acc->irq1);
		if (acc->irq2 != 0)
			disable_irq_nosync(acc->irq2);
		acc->pdata->power_off();
		acc->hw_initialized = 0;
	}
	if (acc->hw_initialized) {
		if (acc->irq1 != 0)
			disable_irq_nosync(acc->irq1);
		if (acc->irq2 != 0)
			disable_irq_nosync(acc->irq2);
		acc->hw_initialized = 0;
	}
}

static int KXTJ1004_acc_device_power_on(struct KXTJ1004_acc_data *acc)
{
	int err = -1;


	if (acc->pdata->power_on) {
		err = acc->pdata->power_on();
		if (err < 0) {
			dev_err(&acc->client->dev,
				"power_on failed: %d\n", err);
			return err;
		}
	}


	if (!acc->hw_initialized) {
		err = KXTJ1004_acc_hw_init(acc);
		if (acc->hw_working == 1 && err < 0) {
			KXTJ1004_acc_device_power_off(acc);
			return err;
		}
	}

	return 0;
}

static irqreturn_t KXTJ1004_acc_isr1(int irq, void *dev)
{
	struct KXTJ1004_acc_data *acc = dev;

	disable_irq_nosync(irq);
	queue_work(acc->irq1_work_queue, &acc->irq1_work);

	pr_debug("%s: isr1 queued\n", KXTJ1004_ACC_DEV_NAME);

	return IRQ_HANDLED;
}

static irqreturn_t KXTJ1004_acc_isr2(int irq, void *dev)
{
	struct KXTJ1004_acc_data *acc = dev;

	disable_irq_nosync(irq);
	queue_work(acc->irq2_work_queue, &acc->irq2_work);

	pr_debug("%s: isr2 queued\n", KXTJ1004_ACC_DEV_NAME);

	return IRQ_HANDLED;
}

static void KXTJ1004_acc_irq1_work_func(struct work_struct *work)
{

	/*struct KXTJ1004_acc_data *acc =
	    container_of(work, struct KXTJ1004_acc_data, irq1_work);
	*/
	/* TODO  add interrupt service procedure.
	   ie:KXTJ1004_acc_get_int1_source(acc); */
	;
	/*  */
	pr_debug("%s: IRQ1 triggered\n", KXTJ1004_ACC_DEV_NAME);
}

static void KXTJ1004_acc_irq2_work_func(struct work_struct *work)
{

	/*struct KXTJ1004_acc_data *acc =
	    container_of(work, struct KXTJ1004_acc_data, irq2_work);
	*/
	/* TODO  add interrupt service procedure.
	   ie:KXTJ1004_acc_get_tap_source(acc); */
	;
	/*  */

	pr_debug("%s: IRQ2 triggered\n", KXTJ1004_ACC_DEV_NAME);
}

int KXTJ1004_acc_update_g_range(struct KXTJ1004_acc_data *acc, u8 new_g_range)
{
	int err;

	u8 sensitivity;
	u8 buf[2];
	u8 updated_val;
	u8 init_val;
	u8 new_val;

	pr_debug("%s\n", __func__);

	switch (new_g_range) {
	case KXTJ1004_ACC_G_2G:

		sensitivity = 4;
		break;
	case KXTJ1004_ACC_G_4G:

		sensitivity = 3;
		break;
	case KXTJ1004_ACC_G_8G:

		sensitivity = 2;
		break;
	default:
		dev_err(&acc->client->dev, "invalid g range requested: %u\n",
			new_g_range);
		return -EINVAL;
	}

	if (atomic_read(&acc->enabled)) {
		/* Set configuration register 4, which contains g range setting
		 *  NOTE: this is a straight overwrite because this driver does
		 *  not use any of the other configuration bits in this
		 *  register.  Should this become untrue, we will have to read
		 *  out the value and only change the relevant bits --XX----
		 *  (marked by X) */
	  acc->ctrl_reg1 &= 0xe7;
	  acc->ctrl_reg1 |= new_g_range;
	  buf[0] = CTRL_REG1;
	  buf[1] = acc->ctrl_reg1;
	  err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	  if (err < 0)
		   dev_err(&acc->client->dev, "soft power off failed\n");
		acc->sensitivity = sensitivity;

		pr_debug("%s sensitivity %d g-range %d\n", __func__, sensitivity,new_g_range);
	}

	return 0;
error:
	dev_err(&acc->client->dev, "update g range failed 0x%x,0x%x: %d\n",
		buf[0], buf[1], err);

	return err;
}

int KXTJ1004_acc_update_odr(struct KXTJ1004_acc_data *acc, int poll_interval_ms)
{
	int err = -1;
	int i;
  u8 buf[2];
#if 0
	for (i = ARRAY_SIZE(lis3dh_acc_odr_table) - 1; i >= 0; i--) {
		if (lis3dh_acc_odr_table[i].cutoff_ms <= poll_interval_ms)
			break;
	}
#endif	
	for (i = 0; i < ARRAY_SIZE(KXTJ1004_acc_odr_table); i++) {
		acc->data_ctrl = KXTJ1004_acc_odr_table[i].mask;
		if (poll_interval_ms < KXTJ1004_acc_odr_table[i].cutoff_ms)
			break;
	}
//err = i2c_smbus_write_byte_data(acc->client, CTRL_REG1, 0);
	buf[0] = CTRL_REG1;
  buf[1] = 0;//acc->ctrl_reg1;
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		return err;

	//err = i2c_smbus_write_byte_data(acc->client, DATA_CTRL, acc->data_ctrl);
	buf[0] = DATA_CTRL;
  buf[1] = acc->data_ctrl;
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		return err;

	buf[0] = CTRL_REG1;
  buf[1] = acc->ctrl_reg1;
	err = KXTJ1004_acc_i2c_write(acc, buf, 1);
	if (err < 0)
		return err;
	
	if (err < 0)
		return err;

	return 0;
}

/* */

static int KXTJ1004_acc_register_write(struct KXTJ1004_acc_data *acc, u8 * buf,
				     u8 reg_address, u8 new_value)
{
	int err = -1;

	if (atomic_read(&acc->enabled)) {
		/* Sets configuration register at reg_address
		 *  NOTE: this is a straight overwrite  */
		buf[0] = reg_address;
		buf[1] = new_value;
		err = KXTJ1004_acc_i2c_write(acc, buf, 1);
		if (err < 0)
			return err;
	}
	return err;
}

static int KXTJ1004_acc_register_read(struct KXTJ1004_acc_data *acc, u8 * buf,
				    u8 reg_address)
{

	int err = -1;
	buf[0] = (reg_address);
	err = KXTJ1004_acc_i2c_read(acc, buf, 1);
	return err;
}

static int KXTJ1004_acc_register_update(struct KXTJ1004_acc_data *acc, u8 * buf,
				      u8 reg_address, u8 mask,
				      u8 new_bit_values)
{
	int err = -1;
	u8 init_val;
	u8 updated_val;
	err = KXTJ1004_acc_register_read(acc, buf, reg_address);
	if (!(err < 0)) {
		init_val = buf[1];
		updated_val = ((mask & new_bit_values) | ((~mask) & init_val));
		err = KXTJ1004_acc_register_write(acc, buf, reg_address,
						updated_val);
	}
	return err;
}

/* */

static int KXTJ1004_acc_get_acceleration_data(struct KXTJ1004_acc_data *acc,
					    int *xyz)
{
	int err = -1;
	/* Data bytes from hardware xL, xH, yL, yH, zL, zH */
	s16 acc_data[3];
	s16 hw_d[3] = { 0 };
	//	acc_data
	/* x,y,z hardware data */
	acc_data[0] = (I2C_AUTO_INCREMENT | XOUT_L);
	err = KXTJ1004_acc_i2c_read(acc, (u8 *)acc_data, 6);
	if (err < 0)
		dev_err(&acc->client->dev, "accelerometer data read failed\n");

	//x = le16_to_cpu(acc_data[tj9->pdata.axis_map_x]);
	//y = le16_to_cpu(acc_data[tj9->pdata.axis_map_y]);
	//z = le16_to_cpu(acc_data[tj9->pdata.axis_map_z]);

	//x >>= tj9->shift;
	//y >>= tj9->shift;
	//z >>= tj9->shift;
	pr_debug("%s read x=%d, y=%d, z=%d\n",
	KXTJ1004_ACC_DEV_NAME, acc_data[0], acc_data[1], acc_data[2]);
	xyz[0] = ((acc->pdata->negate_x) ? (-acc_data[acc->pdata->axis_map_x])
			: (acc_data[acc->pdata->axis_map_x]));
	xyz[1] = ((acc->pdata->negate_y) ? (acc_data[acc->pdata->axis_map_y])
			: (-acc_data[acc->pdata->axis_map_y]));
	xyz[2] = ((acc->pdata->negate_z) ? (acc_data[acc->pdata->axis_map_z])
			: (-acc_data[acc->pdata->axis_map_z]));

	xyz[0]>>= acc->sensitivity;
	xyz[1]>>= acc->sensitivity;
	xyz[2]>>= acc->sensitivity;

#ifdef GSENSOR_CALI_SUPPORT
	xyz[0] -= accel_cali[0];
	xyz[1] -= accel_cali[1];
	xyz[2] -= accel_cali[2];
#endif

	//printk("%s read x=%d, y=%d, z=%d\n", //, DESC_RESP = 0x%x, WAI = 0x%x.\n", 
	//KXTJ1004_ACC_I2C_NAME, xyz[0], xyz[1], xyz[2]); //, acc_data[3] & 0xFF, acc_data[4] >> 8);

	return err;
}

static void KXTJ1004_acc_report_values(struct KXTJ1004_acc_data *acc, int *xyz)
{
	input_report_abs(acc->input_dev, ABS_X, xyz[0]);
	input_report_abs(acc->input_dev, ABS_Y, xyz[1]);
	input_report_abs(acc->input_dev, ABS_Z, xyz[2]);
	input_sync(acc->input_dev);
}

#ifdef GSENSOR_CALI_SUPPORT
static int read_factory_calibration(int *buf);
#endif

static int KXTJ1004_acc_enable(struct KXTJ1004_acc_data *acc)
{
	int err;

	printk("sprd-gsensor: -- %s -- !\n",__func__);

	if (!atomic_cmpxchg(&acc->enabled, 0, 1))
	{
#ifdef GSENSOR_CALI_SUPPORT
		if(KXTJ1004_acc_misc_data->get_cali_flag == 0)
		{
			read_factory_calibration(accel_cali);
			KXTJ1004_acc_misc_data->get_cali_flag = 1;
		}
#endif

		err = KXTJ1004_acc_device_power_on(acc);
		if (err < 0)
		{
			atomic_set(&acc->enabled, 0);
			return err;
		}

		if (acc->hw_initialized)
		{
			if (acc->irq1 != 0)
				enable_irq(acc->irq1);
			if (acc->irq2 != 0)
				enable_irq(acc->irq2);
			pr_debug("%s: power on: irq enabled\n",	KXTJ1004_ACC_DEV_NAME);
		}

		schedule_delayed_work(&acc->input_work,	msecs_to_jiffies(acc->pdata-> poll_interval));
	}

	printk("sprd-gsensor: -- %s -- success!\n",__func__);
	return 0;
}

static int KXTJ1004_acc_disable(struct KXTJ1004_acc_data *acc)
{

	if (atomic_cmpxchg(&acc->enabled, 1, 0)) {
		cancel_delayed_work_sync(&acc->input_work);
		KXTJ1004_acc_device_power_off(acc);
	}

	return 0;
}

static int KXTJ1004_acc_misc_open(struct inode *inode, struct file *file)
{
	int err;
	err = nonseekable_open(inode, file);
	if (err < 0)
		return err;

	file->private_data = KXTJ1004_acc_misc_data;

	return 0;
}

static long KXTJ1004_acc_misc_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	u8 buf[4];
	u8 mask;
	u8 reg_address;
	u8 bit_values;
	int err;
	int interval;
	int xyz[3] = { 0 };
	struct KXTJ1004_acc_data *acc = file->private_data;

	switch (cmd) {
	case KXTJ1004_ACC_IOCTL_GET_DELAY:
		interval = acc->pdata->poll_interval;
		if (copy_to_user(argp, &interval, sizeof(interval)))
			return -EFAULT;
		break;

	case KXTJ1004_ACC_IOCTL_SET_DELAY:
		if (copy_from_user(&interval, argp, sizeof(interval)))
			return -EFAULT;
		if (interval < 0 || interval > 1000)
			return -EINVAL;

		acc->pdata->poll_interval = max(interval,
						acc->pdata->min_interval);
		err = KXTJ1004_acc_update_odr(acc, acc->pdata->poll_interval);
		/* TODO: if update fails poll is still set */
		if (err < 0)
			return err;
		break;

	case KXTJ1004_ACC_IOCTL_SET_ENABLE:
		if (copy_from_user(&interval, argp, sizeof(interval)))
			return -EFAULT;
		if (interval > 1)
			return -EINVAL;
		if (interval)
			err = KXTJ1004_acc_enable(acc);
		else
			err = KXTJ1004_acc_disable(acc);
		return err;
		break;

	case KXTJ1004_ACC_IOCTL_GET_ENABLE:
		interval = atomic_read(&acc->enabled);
		if (copy_to_user(argp, &interval, sizeof(interval)))
			return -EINVAL;
		break;

	case KXTJ1004_ACC_IOCTL_SET_G_RANGE:
		if (copy_from_user(buf, argp, 1))
			return -EFAULT;
		bit_values = buf[0];
		err = KXTJ1004_acc_update_g_range(acc, bit_values);
		if (err < 0)
			return err;
		break;

#if 0//def INTERRUPT_MANAGEMENT
	case KXTJ1004_ACC_IOCTL_SET_CTRL_REG3:
		if (copy_from_user(buf, argp, 2))
			return -EFAULT;
		reg_address = CTRL_REG3;
		mask = buf[1];
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_CTRL_REG3] = ((mask & bit_values) |
						    (~mask & acc->
						     resume_state
						     [RES_CTRL_REG3]));
		break;

	case KXTJ1004_ACC_IOCTL_SET_CTRL_REG6:
		if (copy_from_user(buf, argp, 2))
			return -EFAULT;
		reg_address = CTRL_REG6;
		mask = buf[1];
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_CTRL_REG6] = ((mask & bit_values) |
						    (~mask & acc->
						     resume_state
						     [RES_CTRL_REG6]));
		break;

	case KXTJ1004_ACC_IOCTL_SET_DURATION1:
		if (copy_from_user(buf, argp, 1))
			return -EFAULT;
		reg_address = INT_DUR1;
		mask = 0x7F;
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_INT_DUR1] = ((mask & bit_values) |
						   (~mask & acc->
						    resume_state
						    [RES_INT_DUR1]));
		break;

	case KXTJ1004_ACC_IOCTL_SET_THRESHOLD1:
		if (copy_from_user(buf, argp, 1))
			return -EFAULT;
		reg_address = INT_THS1;
		mask = 0x7F;
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_INT_THS1] = ((mask & bit_values) |
						   (~mask & acc->
						    resume_state
						    [RES_INT_THS1]));
		break;

	case KXTJ1004_ACC_IOCTL_SET_CONFIG1:
		if (copy_from_user(buf, argp, 2))
			return -EFAULT;
		reg_address = INT_CFG1;
		mask = buf[1];
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_INT_CFG1] = ((mask & bit_values) |
						   (~mask & acc->
						    resume_state
						    [RES_INT_CFG1]));
		break;

	case KXTJ1004_ACC_IOCTL_GET_SOURCE1:
		err = KXTJ1004_acc_register_read(acc, buf, INT_SRC1);
		if (err < 0)
			return err;

		pr_debug("INT1_SRC content: %d , 0x%x\n", buf[0], buf[0]);

		if (copy_to_user(argp, buf, 1))
			return -EINVAL;
		break;

	case KXTJ1004_ACC_IOCTL_SET_DURATION2:
		if (copy_from_user(buf, argp, 1))
			return -EFAULT;
		reg_address = INT_DUR2;
		mask = 0x7F;
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_INT_DUR2] = ((mask & bit_values) |
						   (~mask & acc->
						    resume_state
						    [RES_INT_DUR2]));
		break;

	case KXTJ1004_ACC_IOCTL_SET_THRESHOLD2:
		if (copy_from_user(buf, argp, 1))
			return -EFAULT;
		reg_address = INT_THS2;
		mask = 0x7F;
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_INT_THS2] = ((mask & bit_values) |
						   (~mask & acc->
						    resume_state
						    [RES_INT_THS2]));
		break;

	case KXTJ1004_ACC_IOCTL_SET_CONFIG2:
		if (copy_from_user(buf, argp, 2))
			return -EFAULT;
		reg_address = INT_CFG2;
		mask = buf[1];
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_INT_CFG2] = ((mask & bit_values) |
						   (~mask & acc->
						    resume_state
						    [RES_INT_CFG2]));
		break;

	case KXTJ1004_ACC_IOCTL_GET_SOURCE2:
		err = KXTJ1004_acc_register_read(acc, buf, INT_SRC2);
		if (err < 0)
			return err;

		pr_debug("INT2_SRC content: %d , 0x%x\n", buf[0], buf[0]);

		if (copy_to_user(argp, buf, 1))
			return -EINVAL;
		break;

	case KXTJ1004_ACC_IOCTL_GET_TAP_SOURCE:
		err = KXTJ1004_acc_register_read(acc, buf, TT_SRC);
		if (err < 0)
			return err;
		pr_debug("TT_SRC content: %d , 0x%x\n", buf[0], buf[0]);

		if (copy_to_user(argp, buf, 1)) {
			pr_err("%s: %s error in copy_to_user \n",
			       KXTJ1004_ACC_DEV_NAME, __func__);
			return -EINVAL;
		}
		break;

	case KXTJ1004_ACC_IOCTL_SET_TAP_CFG:
		if (copy_from_user(buf, argp, 2))
			return -EFAULT;
		reg_address = TT_CFG;
		mask = buf[1];
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_TT_CFG] = ((mask & bit_values) |
						 (~mask & acc->
						  resume_state[RES_TT_CFG]));
		break;

	case KXTJ1004_ACC_IOCTL_SET_TAP_TLIM:
		if (copy_from_user(buf, argp, 2))
			return -EFAULT;
		reg_address = TT_LIM;
		mask = buf[1];
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_TT_LIM] = ((mask & bit_values) |
						 (~mask & acc->
						  resume_state[RES_TT_LIM]));
		break;

	case KXTJ1004_ACC_IOCTL_SET_TAP_THS:
		if (copy_from_user(buf, argp, 2))
			return -EFAULT;
		reg_address = TT_THS;
		mask = buf[1];
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_TT_THS] = ((mask & bit_values) |
						 (~mask & acc->
						  resume_state[RES_TT_THS]));
		break;

	case KXTJ1004_ACC_IOCTL_SET_TAP_TLAT:
		if (copy_from_user(buf, argp, 2))
			return -EFAULT;
		reg_address = TT_TLAT;
		mask = buf[1];
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_TT_TLAT] = ((mask & bit_values) |
						  (~mask & acc->
						   resume_state[RES_TT_TLAT]));
		break;

	case KXTJ1004_ACC_IOCTL_SET_TAP_TW:
		if (copy_from_user(buf, argp, 2))
			return -EFAULT;
		reg_address = TT_TW;
		mask = buf[1];
		bit_values = buf[0];
		err = KXTJ1004_acc_register_update(acc, (u8 *) arg, reg_address,
						 mask, bit_values);
		if (err < 0)
			return err;
		acc->resume_state[RES_TT_TW] = ((mask & bit_values) |
						(~mask & acc->
						 resume_state[RES_TT_TW]));
		break;

#endif /* INTERRUPT_MANAGEMENT */
  	case KXTJ1004_ACC_IOCTL_GET_COOR_XYZ:
		err = KXTJ1004_acc_get_acceleration_data(acc, xyz);
		if (err < 0)
			return err;

		if (copy_to_user(argp, xyz, sizeof(xyz))) {
			pr_err(" %s %d error in copy_to_user \n",
			       __func__, __LINE__);
			return -EINVAL;
		}
		break;
	case KXTJ1004_ACC_IOCTL_GET_CHIP_ID:
	    {
	        u8 devid = 0;
	        u8 devinfo[DEVICE_INFO_LEN] = {0};
	        err = KXTJ1004_acc_register_read(acc, &devid, WHO_AM_I);
	        if (err < 0) {
	            printk("__func__, error read register WHO_AM_I\n", __func__);
	            return -EAGAIN;
	        }
	        sprintf(devinfo, "%s, %#x", DEVICE_INFO, devid);

	        if (copy_to_user(argp, devinfo, sizeof(devinfo))) {
	            printk("%s error in copy_to_user(IOCTL_GET_CHIP_ID)\n", __func__);
	            return -EINVAL;
	        }
	    }
	   break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct file_operations KXTJ1004_acc_misc_fops = {
	.owner = THIS_MODULE,
	.open = KXTJ1004_acc_misc_open,
	.unlocked_ioctl = KXTJ1004_acc_misc_ioctl,
};

static struct miscdevice KXTJ1004_acc_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = KXTJ1004_ACC_DEV_NAME,
	.fops = &KXTJ1004_acc_misc_fops,
};

static void KXTJ1004_acc_input_work_func(struct work_struct *work)
{
	struct KXTJ1004_acc_data *acc;

	int xyz[3] = { 0 };
	int err;

	acc = container_of((struct delayed_work *)work,
			   struct KXTJ1004_acc_data, input_work);

	mutex_lock(&acc->lock);
	err = KXTJ1004_acc_get_acceleration_data(acc, xyz);
	if (err < 0)
		dev_err(&acc->client->dev, "get_acceleration_data failed\n");
	else
		KXTJ1004_acc_report_values(acc, xyz);

	schedule_delayed_work(&acc->input_work,
			      msecs_to_jiffies(acc->pdata->poll_interval));
	mutex_unlock(&acc->lock);
}

#ifdef KXTJ1004_OPEN_ENABLE
int KXTJ1004_acc_input_open(struct input_dev *input)
{
	struct KXTJ1004_acc_data *acc = input_get_drvdata(input);

	return KXTJ1004_acc_enable(acc);
}

void KXTJ1004_acc_input_close(struct input_dev *dev)
{
	struct KXTJ1004_acc_data *acc = input_get_drvdata(dev);

	KXTJ1004_acc_disable(acc);
}
#endif

static int KXTJ1004_acc_validate_pdata(struct KXTJ1004_acc_data *acc)
{
	acc->pdata->poll_interval = max(acc->pdata->poll_interval,
					acc->pdata->min_interval);

	if (acc->pdata->axis_map_x > 2 || acc->pdata->axis_map_y > 2
	    || acc->pdata->axis_map_z > 2) {
		dev_err(&acc->client->dev, "invalid axis_map value "
			"x:%u y:%u z%u\n", acc->pdata->axis_map_x,
			acc->pdata->axis_map_y, acc->pdata->axis_map_z);
		return -EINVAL;
	}

	/* Only allow 0 and 1 for negation boolean flag */
	if (acc->pdata->negate_x > 1 || acc->pdata->negate_y > 1
	    || acc->pdata->negate_z > 1) {
		dev_err(&acc->client->dev, "invalid negate value "
			"x:%u y:%u z:%u\n", acc->pdata->negate_x,
			acc->pdata->negate_y, acc->pdata->negate_z);
		return -EINVAL;
	}

	/* Enforce minimum polling interval */
	if (acc->pdata->poll_interval < acc->pdata->min_interval) {
		dev_err(&acc->client->dev, "minimum poll interval violated\n");
		return -EINVAL;
	}

	return 0;
}

static int KXTJ1004_acc_input_init(struct KXTJ1004_acc_data *acc)
{
	int err;
	/* Polling rx data when the interrupt is not used.*/
	if (1 /*acc->irq1 == 0 && acc->irq1 == 0 */ ) {
		INIT_DELAYED_WORK(&acc->input_work, KXTJ1004_acc_input_work_func);
	}

	acc->input_dev = input_allocate_device();
	if (!acc->input_dev) {
		err = -ENOMEM;
		dev_err(&acc->client->dev, "input device allocate failed\n");
		goto err0;
	}
#ifdef KXTJ1004_ACC_OPEN_ENABLE
	acc->input_dev->open = KXTJ1004_acc_input_open;
	acc->input_dev->close = KXTJ1004_acc_input_close;
#endif

	input_set_drvdata(acc->input_dev, acc);

	set_bit(EV_ABS, acc->input_dev->evbit);
	/*      next is used for interruptA sources data if the case */
	set_bit(ABS_MISC, acc->input_dev->absbit);
	/*      next is used for interruptB sources data if the case */
	set_bit(ABS_WHEEL, acc->input_dev->absbit);

	input_set_abs_params(acc->input_dev, ABS_X, -G_MAX, G_MAX, FUZZ, FLAT);
	input_set_abs_params(acc->input_dev, ABS_Y, -G_MAX, G_MAX, FUZZ, FLAT);
	input_set_abs_params(acc->input_dev, ABS_Z, -G_MAX, G_MAX, FUZZ, FLAT);
	/*      next is used for interruptA sources data if the case */
	input_set_abs_params(acc->input_dev, ABS_MISC, INT_MIN, INT_MAX, 0, 0);
	/*      next is used for interruptB sources data if the case */
	input_set_abs_params(acc->input_dev, ABS_WHEEL, INT_MIN, INT_MAX, 0, 0);

	acc->input_dev->name = "accelerometer";

	err = input_register_device(acc->input_dev);
	if (err) {
		dev_err(&acc->client->dev,
			"unable to register input polled device %s\n",
			acc->input_dev->name);
		goto err1;
	}

	return 0;

err1:
	input_free_device(acc->input_dev);
err0:
	return err;
}

static void KXTJ1004_acc_input_cleanup(struct KXTJ1004_acc_data *acc)
{
	input_unregister_device(acc->input_dev);
	input_free_device(acc->input_dev);
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void KXTJ1004_early_suspend(struct early_suspend *es);
static void KXTJ1004_early_resume(struct early_suspend *es);
#endif

#ifdef CONFIG_OF
static struct KXTJ1004_acc_platform_data *KXTJ1004_acc_parse_dt(struct device *dev)
{
	struct KXTJ1004_acc_platform_data *pdata;
	struct device_node *np = dev->of_node;
	int ret;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(dev, "Could not allocate struct KXTJ1004_acc_platform_data");
		return NULL;
	}
	ret = of_property_read_u32(np, "poll_interval", &pdata->poll_interval);
	if(ret){
		dev_err(dev, "fail to get poll_interval\n");
		goto fail;
	}
	ret = of_property_read_u32(np, "min_interval", &pdata->min_interval);
	if(ret){
		dev_err(dev, "fail to get min_interval\n");
		goto fail;
	}
	ret = of_property_read_u32(np, "g_range", &pdata->g_range);
	if(ret){
		dev_err(dev, "fail to get g_range\n");
		goto fail;
	}
	ret = of_property_read_u32(np, "axis_map_x", &pdata->axis_map_x);
	if(ret){
		dev_err(dev, "fail to get axis_map_x\n");
		goto fail;
	}
	ret = of_property_read_u32(np, "axis_map_y", &pdata->axis_map_y);
	if(ret){
		dev_err(dev, "fail to get axis_map_y\n");
		goto fail;
	}
	ret = of_property_read_u32(np, "axis_map_z", &pdata->axis_map_z);
	if(ret){
		dev_err(dev, "fail to get axis_map_z\n");
		goto fail;
	}
	ret = of_property_read_u32(np, "negate_x", &pdata->negate_x);
	if(ret){
		dev_err(dev, "fail to get negate_x\n");
		goto fail;
	}
	ret = of_property_read_u32(np, "negate_y", &pdata->negate_y);
	if(ret){
		dev_err(dev, "fail to get negate_y\n");
		goto fail;
	}
	ret = of_property_read_u32(np, "negate_z", &pdata->negate_z);
	if(ret){
		dev_err(dev, "fail to get negate_z\n");
		goto fail;
	}
	return pdata;
fail:
	kfree(pdata);
	return NULL;
}
#endif


#ifdef GSENSOR_CALI_SUPPORT
static int write_factory_calibration(int *buf)
{
    	struct file *fp_cal;
	
	mm_segment_t fs;
	loff_t pos;

	printk("[%s]: start: \n", __func__);
	
    	pos = 0;
		
	fp_cal = filp_open(GSENSOR_CAL_FILE_PATH, O_CREAT|O_RDWR|O_TRUNC, 0660);
	if (IS_ERR(fp_cal))
	{
		printk(" %s create file %s error\n", __func__, GSENSOR_CAL_FILE_PATH);
		return -1;
	}

    	fs = get_fs();
	set_fs(KERNEL_DS);
        
	vfs_write(fp_cal, buf, 12, &pos);
	
    	filp_close(fp_cal, NULL);

	set_fs(fs);
	
	printk("[%s]: end-> %d,%d,%d,%d  \n", __func__,buf[0],buf[1],buf[2],sizeof(buf));
	return 0;
}


static int read_factory_calibration(int *buf)
{
	//struct i2c_client *client = epl_data->client;
	struct file *fp;
	mm_segment_t fs;
	loff_t pos;

	printk("[%s]: start \n", __func__);

	fp = filp_open(GSENSOR_CAL_FILE_PATH, O_RDONLY, (S_IRUSR|S_IRGRP));

	if (IS_ERR(fp))
	{
		printk("open Gsensor calibration file fail!!(%d)\n", (int)IS_ERR(fp));
		return -EINVAL;
	}
	else
	{
		pos = 0;
		fs = get_fs();
		set_fs(KERNEL_DS);
		vfs_read(fp, (char *)buf, 12, &pos);
		filp_close(fp, NULL);
		set_fs(fs);

		printk("[%s]: x =%d, y=%d z=%d\n", __func__, *buf,*(buf+1),*(buf+2));
	}

	printk("[%s]: end \n", __func__);
	return 0;
}

static int gsensor_acc_cali_data(int sample_cnt, int *buf)
{
	int i=0;
	int sum_x=0,sum_y=0,sum_z=0;
	int xyz_data[3];

	if((KXTJ1004_acc_misc_data==NULL) || (buf==NULL) || (sample_cnt==0))
	{
		return -1;
	}

	accel_cali[0]=0;
	accel_cali[1]=0;
	accel_cali[2]=0;

	for(i=0; i<sample_cnt; i++)
	{
		if(KXTJ1004_acc_get_acceleration_data(KXTJ1004_acc_misc_data, &xyz_data[0]) < 0)
		{
			return -1;
		}
		sum_x += xyz_data[0]; //x raw data
		sum_y += xyz_data[1];
		sum_z += xyz_data[2];
		msleep(20);
	}

	*buf++ = sum_x/sample_cnt;
	*buf++ = sum_y/sample_cnt;
	*buf = sum_z/sample_cnt - ((sum_z<0)? (-1024):1024);

	return 0;
}

static ssize_t gsensor_cali_write(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
	int *cali_ptr = accel_cali;

	//printk("%s\n",__func__);

	if(gsensor_acc_cali_data(5, cali_ptr) == 0)
	{
		//printk("%s: x=%d,y=%d,z=%d\n",__func__,*cali_ptr,*(accel_cali+1),*(accel_cali+2));

		if(write_factory_calibration(cali_ptr) < 0)
		{
			return -1;
		}
		else
		{
			KXTJ1004_acc_misc_data->get_cali_flag = 1;
			return count;
		}
	}
	else
	{
		return -1;
	}
}

static ssize_t gsensor_cali_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
	int *cali_ptr = accel_cali;

	if(read_factory_calibration(cali_ptr) < 0)
	{
		printk("%s failed\n", __func__);
		return 0;
	}

	//printk("%s: #### x=%d,y=%d,z=%d\n",__func__,*cali_ptr,*(cali_ptr+1),*(cali_ptr+2));

	return sprintf(buffer,"x=%d,y=%d,z=%d\n",*cali_ptr,*(cali_ptr+1),*(cali_ptr+2));
}

static int gsensor_cali_open(struct inode *inode, struct file *f)
{
	return 0;
}

static int gsensor_cali_release(struct inode *inode, struct file *f)
{
	return 0;
}

static const struct file_operations gsensor_cali_fops =
{
	.owner = THIS_MODULE,
	.open = gsensor_cali_open,
	.write = gsensor_cali_write,
	.read = gsensor_cali_read,
	.release = gsensor_cali_release
};

static struct miscdevice gsensor_cali_struct = {
	.name = "gsensor_cali",
	.fops = &gsensor_cali_fops,
	.minor = MISC_DYNAMIC_MINOR,
};
#endif

static int KXTJ1004_acc_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{

	struct KXTJ1004_acc_data *acc;
	struct KXTJ1004_acc_platform_data *pdata = client->dev.platform_data;

	int err = -1;
	int tempvalue;

	printk("%s: probe start .\n", KXTJ1004_ACC_I2C_NAME);
  
	/*
	if (client->dev.platform_data == NULL) {
		dev_err(&client->dev, "platform data is NULL. exiting.\n");
		err = -ENODEV;
		goto exit_check_functionality_failed;
	}
	*/
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "client not i2c capable\n");
		err = -ENODEV;
		goto exit_check_functionality_failed;
	}


	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE |
				     I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_WORD_DATA)) {
		dev_err(&client->dev, "client not smb-i2c capable:2\n");
		err = -EIO;
		goto exit_check_functionality_failed;
	}


	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_I2C_BLOCK)) {
		dev_err(&client->dev, "client not smb-i2c capable:3\n");
		err = -EIO;
		goto exit_check_functionality_failed;
	}

	/*
	 * OK. From now, we presume we have a valid client. We now create the
	 * client structure, even though we cannot fill it completely yet.
	 */

#if 1	
		tempvalue = i2c_smbus_read_byte_data(client, WHO_AM_I);
		if((tempvalue & 0x00FF) == WHOAMI_KXTJ1004_ACC){
			printk( "%s I2C driver registered!\n", KXTJ1004_ACC_I2C_NAME);
		} else {
		//	acc->client = NULL;
			printk("I2C driver not registered!"
					" Device unknown 0x%x\n", tempvalue);
			goto exit_check_functionality_failed;
		}
	
#endif

#ifdef CONFIG_OF
	struct device_node *np = client->dev.of_node;
	if (np && !pdata){
		pdata = KXTJ1004_acc_parse_dt(&client->dev);
		if(pdata){
			client->dev.platform_data = pdata;
		}
		if(!pdata){
			err = -ENOMEM;
			goto exit_check_functionality_failed;
		}
	}
#endif

	acc = kzalloc(sizeof(struct KXTJ1004_acc_data), GFP_KERNEL);
	if (acc == NULL) {
		err = -ENOMEM;
		dev_err(&client->dev,
			"failed to allocate memory for module data: "
			"%d\n", err);
		goto exit_alloc_data_failed;
	}

	mutex_init(&acc->lock);
	mutex_lock(&acc->lock);

	acc->client = client;
	KXTJ1004_i2c_client = client;
	i2c_set_clientdata(client, acc);

	pr_debug("%s: %s has set irq1 to irq: %d\n",
	       KXTJ1004_ACC_I2C_NAME, __func__, acc->irq1);
	pr_debug("%s: %s has set irq2 to irq: %d\n",
	       KXTJ1004_ACC_I2C_NAME, __func__, acc->irq2);

	//gpio_request(GSENSOR_GINT1_GPI, "GSENSOR_INT1");
	//gpio_request(GSENSOR_GINT2_GPI, "GSENSOR_INT2");
	acc->irq1 = 0; /* gpio_to_irq(GSENSOR_GINT1_GPI); */
	acc->irq2 = 0; /* gpio_to_irq(GSENSOR_GINT2_GPI); */

	if (acc->irq1 != 0) {
		pr_debug("%s request irq1\n", __func__);
		err =
		    request_irq(acc->irq1, KXTJ1004_acc_isr1, IRQF_TRIGGER_RISING,
				"KXTJ1004_acc_irq1", acc);
		if (err < 0) {
			dev_err(&client->dev, "request irq1 failed: %d\n", err);
			goto err_mutexunlockfreedata;
		}

		disable_irq_nosync(acc->irq1);

		INIT_WORK(&acc->irq1_work, KXTJ1004_acc_irq1_work_func);
		acc->irq1_work_queue =
		    create_singlethread_workqueue("KXTJ1004_acc_wq1");
		if (!acc->irq1_work_queue) {
			err = -ENOMEM;
			dev_err(&client->dev, "cannot create work queue1: %d\n",
				err);
			goto err_free_irq1;
		}

	}


	if (acc->irq2 != 0) {
		err =
		    request_irq(acc->irq2, KXTJ1004_acc_isr2, IRQF_TRIGGER_RISING,
				"KXTJ1004_acc_irq2", acc);
		if (err < 0) {
			dev_err(&client->dev, "request irq2 failed: %d\n", err);
			goto err_destoyworkqueue1;
		}

		disable_irq_nosync(acc->irq2);

/*		 Create workqueue for IRQ.*/

		INIT_WORK(&acc->irq2_work, KXTJ1004_acc_irq2_work_func);
		acc->irq2_work_queue =
		    create_singlethread_workqueue("KXTJ1004_acc_wq2");
		if (!acc->irq2_work_queue) {
			err = -ENOMEM;
			dev_err(&client->dev, "cannot create work queue2: %d\n",
				err);
			goto err_free_irq2;
		}

	}


	acc->pdata = kmalloc(sizeof(*acc->pdata), GFP_KERNEL);
	if (acc->pdata == NULL) {
		err = -ENOMEM;
		dev_err(&client->dev,
			"failed to allocate memory for pdata: %d\n", err);
		goto err_destoyworkqueue2;
	}

	memcpy(acc->pdata, client->dev.platform_data, sizeof(*acc->pdata));

	err = KXTJ1004_acc_validate_pdata(acc);
	if (err < 0) {
		dev_err(&client->dev, "failed to validate platform data\n");
		goto exit_kfree_pdata;
	}

	//acc->client->addr = KXTJ1004_ACC_I2C_ADDR;
	
	i2c_set_clientdata(client, acc);

	if (acc->pdata->init) {
		err = acc->pdata->init();
		if (err < 0) {
			dev_err(&client->dev, "init failed: %d\n", err);
			goto err2;
		}

	}

	err = KXTJ1004_acc_device_power_on(acc);
	if (err < 0) {
		dev_err(&client->dev, "power on failed: %d\n", err);
		goto err2;
	}

	memset(acc->resume_state, 0, ARRAY_SIZE(acc->resume_state));
#if 0
	acc->resume_state[RES_CTRL_REG1] = KXTJ1004_ACC_ENABLE_ALL_AXES;
	acc->resume_state[RES_CTRL_REG2] = 0x00;
	acc->resume_state[RES_CTRL_REG3] = 0x00;
	acc->resume_state[RES_CTRL_REG4] = 0x00;
	acc->resume_state[RES_CTRL_REG5] = 0x00;
	acc->resume_state[RES_CTRL_REG6] = 0x00;

	acc->resume_state[RES_TEMP_CFG_REG] = 0x00;
	acc->resume_state[RES_FIFO_CTRL_REG] = 0x00;
	acc->resume_state[RES_INT_CFG1] = 0x00;
	acc->resume_state[RES_INT_THS1] = 0x00;
	acc->resume_state[RES_INT_DUR1] = 0x00;
	acc->resume_state[RES_INT_CFG2] = 0x00;
	acc->resume_state[RES_INT_THS2] = 0x00;
	acc->resume_state[RES_INT_DUR2] = 0x00;

	acc->resume_state[RES_TT_CFG] = 0x00;
	acc->resume_state[RES_TT_THS] = 0x00;
	acc->resume_state[RES_TT_LIM] = 0x00;
	acc->resume_state[RES_TT_TLAT] = 0x00;
	acc->resume_state[RES_TT_TW] = 0x00;
#endif

	atomic_set(&acc->enabled, 1);

	err = KXTJ1004_acc_input_init(acc);
	if (err < 0) {
		dev_err(&client->dev, "input init failed\n");
		goto err_power_off;
	}
	KXTJ1004_acc_misc_data = acc;


	err = misc_register(&KXTJ1004_acc_misc_device);
	if (err < 0) {
		dev_err(&client->dev,
			"misc KXTJ1004_ACC_DEV_NAME register failed\n");
		goto err_input_cleanup;
	}


	KXTJ1004_acc_device_power_off(acc);

	/* As default, do not report information */
	atomic_set(&acc->enabled, 0);
/*
#ifdef CONFIG_HAS_EARLYSUSPEND
	acc->early_suspend.suspend = KXTJ1004_early_suspend;
	acc->early_suspend.resume = KXTJ1004_early_resume;
	acc->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
	register_early_suspend(&acc->early_suspend);
#endif
*/

#ifdef GSENSOR_CALI_SUPPORT
	KXTJ1004_acc_misc_data->get_cali_flag = 0;
	if (misc_register(&gsensor_cali_struct) < 0)
	{
		printk("%s: Creat gsensor_cali_struct device file error!!\n", __func__);
	}
#endif

	mutex_unlock(&acc->lock);
	dev_info(&client->dev, "###%s###\n", __func__);
	printk("sprd-gsensor: -- %s -- success !\n",__func__);
	return 0;

err_input_cleanup:
	KXTJ1004_acc_input_cleanup(acc);

err_power_off:
	KXTJ1004_acc_device_power_off(acc);

err2:
	if (acc->pdata->exit)
		acc->pdata->exit();

exit_kfree_pdata:
	kfree(acc->pdata);

err_destoyworkqueue2:
	if (acc->irq2_work_queue)
		destroy_workqueue(acc->irq2_work_queue);

err_free_irq2:
	if (acc->irq2) {
		free_irq(acc->irq2, acc);
//		gpio_free(GSENSOR_GINT2_GPI);
	}

err_destoyworkqueue1:
	if (acc->irq1_work_queue)
		destroy_workqueue(acc->irq1_work_queue);

err_free_irq1:
	if (acc->irq1) {
		free_irq(acc->irq1, acc);
//		gpio_free(GSENSOR_GINT1_GPI);
	}

err_mutexunlockfreedata:
	i2c_set_clientdata(client, NULL);
	mutex_unlock(&acc->lock);
	kfree(acc);
	KXTJ1004_acc_misc_data = NULL;

exit_alloc_data_failed:
exit_check_functionality_failed:
	pr_err("%s: Driver Init failed\n", KXTJ1004_ACC_I2C_NAME);

	return err;
}

static int  KXTJ1004_acc_remove(struct i2c_client *client)
{
	/* TODO: revisit ordering here once _probe order is finalized */

	struct KXTJ1004_acc_data *acc = i2c_get_clientdata(client);
	if (acc != NULL) {
		if (acc->irq1) {
			free_irq(acc->irq1, acc);
			//gpio_free(GSENSOR_GINT1_GPI);
		}
		if (acc->irq2) {
			free_irq(acc->irq2, acc);
			//gpio_free(GSENSOR_GINT2_GPI);
		}

		if (acc->irq1_work_queue)
			destroy_workqueue(acc->irq1_work_queue);
		if (acc->irq2_work_queue)
			destroy_workqueue(acc->irq2_work_queue);
		misc_deregister(&KXTJ1004_acc_misc_device);
		KXTJ1004_acc_input_cleanup(acc);
		KXTJ1004_acc_device_power_off(acc);
		if (acc->pdata->exit)
			acc->pdata->exit();
		kfree(acc->pdata);
		kfree(acc);
	}

	return 0;
}

static int KXTJ1004_acc_resume(struct i2c_client *client)
{
	struct KXTJ1004_acc_data *acc = i2c_get_clientdata(client);
	//printk("sprd-gsensor: -- %s -- !\n",__func__);
	dev_dbg(&client->dev, "###%s###\n", __func__);

	if (acc != NULL && acc->on_before_suspend)
		return KXTJ1004_acc_enable(acc);

	return 0;
}

static int KXTJ1004_acc_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct KXTJ1004_acc_data *acc = i2c_get_clientdata(client);
	//printk("sprd-gsensor: -- %s -- !\n",__func__);
	dev_dbg(&client->dev, "###%s###\n", __func__);

	if (acc != NULL) {
		acc->on_before_suspend = atomic_read(&acc->enabled);
		return KXTJ1004_acc_disable(acc);
	}
	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND

static void KXTJ1004_early_suspend(struct early_suspend *es)
{
	KXTJ1004_acc_suspend(KXTJ1004_i2c_client, (pm_message_t) {
			   .event = 0});
}

static void KXTJ1004_early_resume(struct early_suspend *es)
{
	KXTJ1004_acc_resume(KXTJ1004_i2c_client);
}

#endif /* CONFIG_HAS_EARLYSUSPEND */

static const struct i2c_device_id KXTJ1004_acc_id[]
= { {KXTJ1004_ACC_I2C_NAME, 0}, {}, };

MODULE_DEVICE_TABLE(i2c, KXTJ1004_acc_id);

static const struct of_device_id kxtj1004_acc_of_match[] ={
	{ .compatible = "Konix,kxtj1004_acc", },
	{ }
};
MODULE_DEVICE_TABLE(of, kxtj1004_acc_of_match);

static struct i2c_driver KXTJ1004_acc_driver = {
	.driver = {
		   .name = KXTJ1004_ACC_I2C_NAME,
		   .of_match_table = kxtj1004_acc_of_match,
		   },
	.probe = KXTJ1004_acc_probe,
	.remove = KXTJ1004_acc_remove,
	.resume = KXTJ1004_acc_resume,
	.suspend = KXTJ1004_acc_suspend,
	.id_table = KXTJ1004_acc_id,
};


static int __init KXTJ1004_acc_init(void)
{
	printk("%s accelerometer driver: init\n", KXTJ1004_ACC_I2C_NAME);

	return i2c_add_driver(&KXTJ1004_acc_driver);
}

static void __exit KXTJ1004_acc_exit(void)
{
	printk("%s accelerometer driver exit\n", KXTJ1004_ACC_I2C_NAME);

	i2c_del_driver(&KXTJ1004_acc_driver);
	return;
}

module_init(KXTJ1004_acc_init);
module_exit(KXTJ1004_acc_exit);

MODULE_DESCRIPTION("KXTJ1004 accelerometer misc driver");
MODULE_AUTHOR("STMicroelectronics");
MODULE_LICENSE("GPL");
