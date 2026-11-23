// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SAMSUNG NFC Controller
 *
 * Copyright (C) 2013 Samsung Electronics Co.Ltd
 * Author: Woonki Lee <woonki84.lee@samsung.com>
 *         Heejae Kim <heejae12.kim@samsung.com>
 */

#include <linux/wait.h>
#include <linux/delay.h>

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/gpio/consumer.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/i2c.h>

#include "sec_nfc.h"

#define SEC_NFC_GET_INFO(dev) i2c_get_clientdata(to_i2c_client(dev))
enum sec_nfc_irq {
	SEC_NFC_SKIP = -1,
	SEC_NFC_NONE,
	SEC_NFC_INT,
	SEC_NFC_READ_TIMES,
};

struct sec_nfc_platform_data {
	struct gpio_desc *ven;
	struct gpio_desc *firm;		/* also the wake pin */
	struct gpio_desc *irq;
	struct gpio_desc *clk_req;	/* only with sec-nfc,nfc_ap_clk */
	int clk_irq;
	struct clk *clk;		/* only with sec-nfc,nfc_ap_clk */
	struct regulator *pvdd;		/* only with sec-nfc,ldo_control */
	const char *nfc_pvdd;		/* the rail name, for the state log */
};

struct sec_nfc_i2c_info {
	struct i2c_client *i2c_dev;
	struct mutex read_mutex;
	enum sec_nfc_irq read_irq;
	wait_queue_head_t read_wait;
	size_t buflen;
	u8 *buf;
};

struct sec_nfc_info {
	struct miscdevice miscdev;
	struct mutex mutex;
	enum sec_nfc_mode mode;
	struct device *dev;
	struct sec_nfc_platform_data *pdata;
	struct sec_nfc_i2c_info i2c_info;
	struct wakeup_source *ws;
	bool clk_ctl;
	bool clk_state;
};

static irqreturn_t sec_nfc_irq_thread_fn(int irq, void *dev_id)
{
	struct sec_nfc_info *info = dev_id;
	struct sec_nfc_platform_data *pdata = info->pdata;

	NFC_LOG_REC("irq\n");

	if (gpiod_get_value_cansleep(pdata->irq) == 0) {
		NFC_LOG_REC("irq-gpio state is low!\n");
		return IRQ_HANDLED;
	}
	mutex_lock(&info->i2c_info.read_mutex);
	/* Skip interrupt during power switching
	 * It is released after first write */
	if (info->i2c_info.read_irq == SEC_NFC_SKIP) {
		NFC_LOG_REC("Now power swiching. Skip this IRQ\n");
		mutex_unlock(&info->i2c_info.read_mutex);
		return IRQ_HANDLED;
	}

	info->i2c_info.read_irq += SEC_NFC_READ_TIMES;
	mutex_unlock(&info->i2c_info.read_mutex);

	wake_up_interruptible(&info->i2c_info.read_wait);
	__pm_wakeup_event(info->ws, 2000);

	return IRQ_HANDLED;
}

static ssize_t sec_nfc_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct sec_nfc_info *info = container_of(file->private_data,
						struct sec_nfc_info, miscdev);
	enum sec_nfc_irq irq;
	int ret = 0;

	mutex_lock(&info->mutex);

	if (info->mode == SEC_NFC_MODE_OFF) {
		NFC_LOG_ERR("read() nfc is not enabled\n");
		ret = -ENODEV;
		goto out;
	}

	mutex_lock(&info->i2c_info.read_mutex);
	if (count == 0)	{
		if (info->i2c_info.read_irq >= SEC_NFC_INT)
			info->i2c_info.read_irq--;
		mutex_unlock(&info->i2c_info.read_mutex);
		goto out;
	}

	irq = info->i2c_info.read_irq;
	mutex_unlock(&info->i2c_info.read_mutex);
	if (irq == SEC_NFC_NONE) {
		if (file->f_flags & O_NONBLOCK) {
			NFC_LOG_ERR("read() it is nonblock\n");
			ret = -EAGAIN;
			goto out;
		}
	}

	/* i2c recv */
	if (count > info->i2c_info.buflen)
		count = info->i2c_info.buflen;

	if (count > SEC_NFC_MSG_MAX_SIZE) {
		NFC_LOG_ERR("read() user required wrong size :%d\n", (u32)count);
		ret = -EINVAL;
		goto out;
	}

	NFC_LOG_REC("read(%zu)\n", count);
	mutex_lock(&info->i2c_info.read_mutex);
	memset(info->i2c_info.buf, 0, count);
	ret = i2c_master_recv(info->i2c_info.i2c_dev, info->i2c_info.buf, (u32)count);

	if (ret == -EREMOTEIO) {
		ret = -ERESTART;
		goto read_error;
	} else if (ret != count) {
		NFC_LOG_ERR("read failed: return: %d count: %d\n",
			ret, (u32)count);
		/*ret = -EREMOTEIO;*/
		goto read_error;
	}

	if (info->i2c_info.read_irq >= SEC_NFC_INT)
		info->i2c_info.read_irq--;

	if (info->i2c_info.read_irq == SEC_NFC_READ_TIMES)
		wake_up_interruptible(&info->i2c_info.read_wait);

	mutex_unlock(&info->i2c_info.read_mutex);

	if (copy_to_user(buf, info->i2c_info.buf, ret)) {
		NFC_LOG_ERR("read() copy failed to user\n");
		ret = -EFAULT;
	}

	goto out;

read_error:
	NFC_LOG_ERR("read error %d\n", ret);
	info->i2c_info.read_irq = SEC_NFC_NONE;
	mutex_unlock(&info->i2c_info.read_mutex);
out:
	mutex_unlock(&info->mutex);

	return ret;
}

static ssize_t sec_nfc_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct sec_nfc_info *info = container_of(file->private_data,
						struct sec_nfc_info, miscdev);
	int ret = 0;

	NFC_LOG_DBG("write() count %d\n", (u32)count);

	mutex_lock(&info->mutex);

	if (info->mode == SEC_NFC_MODE_OFF) {
		NFC_LOG_ERR("write() nfc is not enabled\n");
		ret = -ENODEV;
		goto out;
	}

	if (count > info->i2c_info.buflen)
		count = info->i2c_info.buflen;

	if (count > SEC_NFC_MSG_MAX_SIZE) {
		NFC_LOG_ERR("write() user required wrong size :%d\n", (u32)count);
		ret = -EINVAL;
		goto out;
	}

	if (copy_from_user(info->i2c_info.buf, buf, count)) {
		NFC_LOG_ERR("write() copy failed from user\n");
		ret = -EFAULT;
		goto out;
	}

	/* Skip interrupt during power switching
	 * It is released after first write */
	NFC_LOG_REC("write(%d)\n", count);
	mutex_lock(&info->i2c_info.read_mutex);
	ret = i2c_master_send(info->i2c_info.i2c_dev, info->i2c_info.buf, count);
	if (info->i2c_info.read_irq == SEC_NFC_SKIP)
		info->i2c_info.read_irq = SEC_NFC_NONE;
	mutex_unlock(&info->i2c_info.read_mutex);

	if (ret == -EREMOTEIO) {
		NFC_LOG_ERR("write failed: return: %d count: %d\n",
			ret, (u32)count);
		ret = -ERESTART;
		goto out;
	}

	if (ret != count) {
		NFC_LOG_ERR("write failed: return: %d count: %d\n",
			ret, (u32)count);
		ret = -EREMOTEIO;
	}

out:
	mutex_unlock(&info->mutex);

	return ret;
}

static __poll_t sec_nfc_poll(struct file *file, poll_table *wait)
{
	struct sec_nfc_info *info = container_of(file->private_data,
						struct sec_nfc_info, miscdev);
	enum sec_nfc_irq irq;

	__poll_t ret = 0;

	mutex_lock(&info->mutex);

	if (info->mode == SEC_NFC_MODE_OFF) {
		NFC_LOG_ERR("poll() nfc is not enabled\n");
		/* the vendor driver returned -ENODEV here; select() sees both as readable */
		ret = EPOLLERR | EPOLLHUP;
		goto out;
	}

	poll_wait(file, &info->i2c_info.read_wait, wait);

	mutex_lock(&info->i2c_info.read_mutex);
	irq = info->i2c_info.read_irq;
	if (irq == SEC_NFC_READ_TIMES)
		ret = (EPOLLIN | EPOLLRDNORM);
	mutex_unlock(&info->i2c_info.read_mutex);

out:
	mutex_unlock(&info->mutex);

	return ret;
}

static int sec_nfc_regulator_onoff(struct sec_nfc_platform_data *data, int onoff)
{
	int rc = 0;

	NFC_LOG_INFO("regulator onoff = %d\n", onoff);

	if (onoff == NFC_I2C_LDO_ON) {
		rc = regulator_enable(data->pvdd);
		if (rc) {
			NFC_LOG_ERR("regulator enable nfc_pvdd failed, rc=%d\n", rc);
			goto done;
		}
	} else {
		rc = regulator_disable(data->pvdd);
		if (rc) {
			NFC_LOG_ERR("regulator disable nfc_pvdd failed, rc=%d\n", rc);
			goto done;
		}
	}

	done:

	return rc;
}

static irqreturn_t sec_nfc_clk_irq_thread(int irq, void *dev_id)
{
	struct sec_nfc_info *info = dev_id;
	struct sec_nfc_platform_data *pdata = info->pdata;
	bool value;

	value = gpiod_get_value_cansleep(pdata->clk_req) > 0 ? true : false;
	NFC_LOG_REC("clock req: %d\n", value);

	if (value == info->clk_state)
		return IRQ_HANDLED;

	if (value) {
		int ret = clk_prepare_enable(pdata->clk);

		if (ret) {
			NFC_LOG_ERR("clock enable failed\n");
			return IRQ_HANDLED;
		}
	} else
		clk_disable_unprepare(pdata->clk);

	info->clk_state = value;

	return IRQ_HANDLED;
}

static void sec_nfc_clk_ctl_enable(struct sec_nfc_info *info)
{
	struct sec_nfc_platform_data *pdata = info->pdata;

	if (info->clk_ctl)
		return;

	if (!pdata->clk)
		return;

	info->clk_state = false;
	enable_irq(pdata->clk_irq);
	info->clk_ctl = true;
}

static void sec_nfc_clk_ctl_disable(struct sec_nfc_info *info)
{
	struct sec_nfc_platform_data *pdata = info->pdata;

	if (!info->clk_ctl)
		return;

	if (!pdata->clk)
		return;

	disable_irq(pdata->clk_irq);
	if (info->clk_state)
		clk_disable_unprepare(pdata->clk);

	info->clk_state = false;
	info->clk_ctl = false;
}

static void sec_nfc_set_mode(struct sec_nfc_info *info,
					enum sec_nfc_mode mode)
{
	struct sec_nfc_platform_data *pdata = info->pdata;

	/* intfo lock is aleady gotten before calling this function */
	if (info->mode == mode) {
		NFC_LOG_DBG("power mode is already %d", mode);
		return;
	}
	info->mode = mode;

	/* Skip interrupt during power switching
	 * It is released after first write */
	mutex_lock(&info->i2c_info.read_mutex);
	info->i2c_info.read_irq = SEC_NFC_SKIP;
	mutex_unlock(&info->i2c_info.read_mutex);

	gpiod_set_value_cansleep(pdata->ven, SEC_NFC_PW_OFF);
	gpiod_set_value_cansleep(pdata->firm, SEC_NFC_FW_OFF);

	if (mode == SEC_NFC_MODE_BOOTLOADER)
		gpiod_set_value_cansleep(pdata->firm, SEC_NFC_FW_ON);

	if (mode != SEC_NFC_MODE_OFF) {
		msleep(SEC_NFC_VEN_WAIT_TIME);
		gpiod_set_value_cansleep(pdata->ven, SEC_NFC_PW_ON);
		sec_nfc_clk_ctl_enable(info);
		enable_irq_wake(info->i2c_info.i2c_dev->irq);
		msleep(SEC_NFC_VEN_WAIT_TIME/2);
	} else {
		sec_nfc_clk_ctl_disable(info);
		disable_irq_wake(info->i2c_info.i2c_dev->irq);
	}

	if (info->ws->active)
		__pm_relax(info->ws);

	NFC_LOG_INFO("NFC mode is : %d\n", mode);
}

static long sec_nfc_ioctl(struct file *file, unsigned int cmd,
							unsigned long arg)
{
	struct sec_nfc_info *info = container_of(file->private_data,
						struct sec_nfc_info, miscdev);
	struct sec_nfc_platform_data *pdata = info->pdata;
	unsigned int new = (unsigned int)arg;
	int ret = 0;

	NFC_LOG_DBG("cmd: 0x%x\n", cmd);

	mutex_lock(&info->mutex);

	switch (cmd) {
	case SEC_NFC_DEBUG:
		break;
	case SEC_NFC_SET_MODE:
		if (info->mode == new)
			break;

		if (new >= SEC_NFC_MODE_COUNT) {
			NFC_LOG_ERR("wrong mode (%d)\n", new);
			ret = -EFAULT;
			break;
		}
		sec_nfc_set_mode(info, new);

		break;

	case SEC_NFC_SLEEP:
		if (info->mode != SEC_NFC_MODE_BOOTLOADER) {
			if (info->ws->active)
				__pm_relax(info->ws);
			gpiod_set_value_cansleep(pdata->firm, SEC_NFC_WAKE_SLEEP);
		}
		break;

	case SEC_NFC_WAKEUP:
		if (info->mode != SEC_NFC_MODE_BOOTLOADER) {
			gpiod_set_value_cansleep(pdata->firm, SEC_NFC_WAKE_UP);
			if (!info->ws->active)
				__pm_stay_awake(info->ws);
		}
		break;

// [START] NPT
	case SEC_NFC_SET_NPT_MODE:
		NFC_LOG_INFO("NPT: VEN=%d, FIRM:%d\n", gpiod_get_value_cansleep(pdata->ven),
			     gpiod_get_value_cansleep(pdata->firm));

		if (new == SEC_NFC_NPT_CMD_ON) {
			NFC_LOG_INFO("NPT: NFC OFF mode NPT - Turn on VEN.\n");
			info->mode = SEC_NFC_MODE_FIRMWARE;
			mutex_lock(&info->i2c_info.read_mutex);
			info->i2c_info.read_irq = SEC_NFC_SKIP;
			mutex_unlock(&info->i2c_info.read_mutex);
			gpiod_set_value_cansleep(pdata->ven, SEC_NFC_PW_ON);
			sec_nfc_clk_ctl_enable(info);
			msleep(20);
			gpiod_set_value_cansleep(pdata->firm, SEC_NFC_FW_ON);
			enable_irq_wake(info->i2c_info.i2c_dev->irq);
		} else if (new == SEC_NFC_NPT_CMD_OFF) {
			NFC_LOG_INFO("NPT: NFC OFF mode NPT - Turn off VEN.\n");
			info->mode = SEC_NFC_MODE_OFF;
			gpiod_set_value_cansleep(pdata->firm, SEC_NFC_FW_OFF);
			gpiod_set_value_cansleep(pdata->ven, SEC_NFC_PW_OFF);
			sec_nfc_clk_ctl_disable(info);
			disable_irq_wake(info->i2c_info.i2c_dev->irq);
		}
		break;
// [END] NPT

	default:
		NFC_LOG_ERR("NPT: Unknown ioctl 0x%x\n", cmd);
		ret = -ENOIOCTLCMD;
		break;
	}

	mutex_unlock(&info->mutex);

	return ret;
}

static int nfc_state_print(struct sec_nfc_info *info)
{
	struct sec_nfc_platform_data *pdata = info->pdata;

	int en = gpiod_get_value_cansleep(pdata->ven);
	int firm = gpiod_get_value_cansleep(pdata->firm);
	int pvdd = 0;

	if (!pdata->pvdd) {
		pr_err("%s - nfc_pvdd regulator_get fail\n", __func__);
		return -ENODEV;
	}
	pvdd = regulator_is_enabled(pdata->pvdd);

	NFC_LOG_INFO("%s en: %d, firm: %d power: %d\n", __func__, en, firm, pvdd);
	NFC_LOG_INFO("%s mode %d, clk_state: %d\n", __func__, info->mode, info->clk_state);

	return 0;
}

static int sec_nfc_open(struct inode *inode, struct file *file)
{
	struct sec_nfc_info *info = container_of(file->private_data,
						struct sec_nfc_info, miscdev);
	int ret = 0;

	NFC_LOG_INFO("%s\n", __func__);

	mutex_lock(&info->mutex);
	if (info->mode != SEC_NFC_MODE_OFF) {
		NFC_LOG_ERR("open() nfc is busy\n");
		ret = -EBUSY;
		goto out;
	}

	sec_nfc_set_mode(info, SEC_NFC_MODE_OFF);

out:
	mutex_unlock(&info->mutex);
	return ret;
}

static int sec_nfc_close(struct inode *inode, struct file *file)
{
	struct sec_nfc_info *info = container_of(file->private_data,
						struct sec_nfc_info, miscdev);

	nfc_state_print(info);

	NFC_LOG_INFO("%s\n", __func__);

	mutex_lock(&info->mutex);
	sec_nfc_set_mode(info, SEC_NFC_MODE_OFF);
	mutex_unlock(&info->mutex);

	return 0;
}

static const struct file_operations sec_nfc_fops = {
	.owner		= THIS_MODULE,
	.read		= sec_nfc_read,
	.write		= sec_nfc_write,
	.poll		= sec_nfc_poll,
	.open		= sec_nfc_open,
	.release	= sec_nfc_close,
	.unlocked_ioctl	= sec_nfc_ioctl,
};

static int sec_nfc_suspend(struct device *dev)
{
	struct sec_nfc_info *info = SEC_NFC_GET_INFO(dev);
	int ret = 0;

	mutex_lock(&info->mutex);

	if (info->mode == SEC_NFC_MODE_BOOTLOADER)
		ret = -EPERM;

	mutex_unlock(&info->mutex);

	return ret;
}

static int sec_nfc_resume(struct device *dev)
{
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(sec_nfc_pm_ops, sec_nfc_suspend, sec_nfc_resume);

/* VEN starts high (off), FIRM low, as the vendor driver set them */
static int sec_nfc_parse_dt(struct device *dev,
	struct sec_nfc_platform_data *pdata)
{
	struct device_node *np = dev->of_node;

	pdata->ven = devm_gpiod_get(dev, "sec-nfc,ven", GPIOD_OUT_HIGH);
	if (IS_ERR(pdata->ven)) {
		NFC_LOG_ERR("probe() failed to get gpio ven\n");
		return PTR_ERR(pdata->ven);
	}
	pdata->firm = devm_gpiod_get(dev, "sec-nfc,firm", GPIOD_OUT_LOW);
	if (IS_ERR(pdata->firm)) {
		NFC_LOG_ERR("probe() failed to get gpio firm\n");
		return PTR_ERR(pdata->firm);
	}
	pdata->irq = devm_gpiod_get(dev, "sec-nfc,irq", GPIOD_IN);
	if (IS_ERR(pdata->irq)) {
		NFC_LOG_ERR("probe() GPIO request is failed to register IRQ\n");
		return PTR_ERR(pdata->irq);
	}

	if (of_property_read_bool(np, "sec-nfc,nfc_ap_clk")) {
		pdata->clk_req = devm_gpiod_get(dev, "sec-nfc,clk_req", GPIOD_IN);
		if (IS_ERR(pdata->clk_req))
			return PTR_ERR(pdata->clk_req);
		NFC_LOG_INFO("parse_dt() clk_req : %d\n", desc_to_gpio(pdata->clk_req));
	}

	if (of_property_read_bool(np, "sec-nfc,ldo_control")) {
		if (of_property_read_string(np, "sec-nfc,nfc_pvdd",
					&pdata->nfc_pvdd) < 0) {
			NFC_LOG_ERR("parse_dt() get nfc_pvdd error\n");
			pdata->nfc_pvdd = NULL;
		}
		pdata->pvdd = devm_regulator_get(dev, "nfc-pvdd");
		if (IS_ERR(pdata->pvdd)) {
			NFC_LOG_ERR("nfc_pvdd regulator_get fail\n");
			return PTR_ERR(pdata->pvdd);
		}
	}

	NFC_LOG_INFO("parse_dt() irq : %d, ven : %d, firm : %d\n",
			desc_to_gpio(pdata->irq), desc_to_gpio(pdata->ven),
			desc_to_gpio(pdata->firm));

	return 0;
}

static void sec_nfc_ws_unregister(void *data)
{
	wakeup_source_unregister(data);
}

static void sec_nfc_pvdd_off(void *data)
{
	sec_nfc_regulator_onoff(data, NFC_I2C_LDO_OFF);
}

static int sec_nfc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sec_nfc_platform_data *pdata;
	struct sec_nfc_info *info;
	int ret;

	NFC_LOG_INFO("probe start\n");

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		NFC_LOG_ERR("probe() Failed to allocate memory\n");
		return -ENOMEM;
	}
	ret = sec_nfc_parse_dt(dev, pdata);
	if (ret)
		return ret;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info) {
		NFC_LOG_ERR("probe() failed to allocate memory for sec_nfc_info\n");
		return -ENOMEM;
	}
	info->dev = dev;
	info->pdata = pdata;
	info->mode = SEC_NFC_MODE_OFF;
	mutex_init(&info->mutex);

	info->i2c_info.buflen = SEC_NFC_MAX_BUFFER_SIZE;
	info->i2c_info.buf = devm_kzalloc(dev, SEC_NFC_MAX_BUFFER_SIZE, GFP_KERNEL);
	if (!info->i2c_info.buf) {
		NFC_LOG_ERR("probe() failed to allocate memory\n");
		return -ENOMEM;
	}
	info->i2c_info.i2c_dev = client;
	info->i2c_info.read_irq = SEC_NFC_NONE;
	mutex_init(&info->i2c_info.read_mutex);
	init_waitqueue_head(&info->i2c_info.read_wait);
	i2c_set_clientdata(client, info);

	info->ws = wakeup_source_register(dev, "nfc_wake_lock");
	if (!info->ws)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, sec_nfc_ws_unregister, info->ws);
	if (ret)
		return ret;

	if (pdata->clk_req) {
		pdata->clk = devm_clk_get(dev, "oscclk_nfc");
		if (IS_ERR(pdata->clk)) {
			NFC_LOG_ERR("probe() clk not found\n");
			return PTR_ERR(pdata->clk);
		}
		pdata->clk_irq = gpiod_to_irq(pdata->clk_req);
		if (pdata->clk_irq < 0)
			return pdata->clk_irq;

		ret = devm_request_threaded_irq(dev, pdata->clk_irq, NULL,
						sec_nfc_clk_irq_thread,
						IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
						IRQF_ONESHOT | IRQF_NO_AUTOEN,
						SEC_NFC_DRIVER_NAME, info);
		if (ret < 0) {
			NFC_LOG_ERR("probe() failed to register CLK REQ IRQ handler\n");
			return ret;
		}
	}

	NFC_LOG_INFO("probe() success\n");

	NFC_LOG_INFO("probe() start\n");

	ret = devm_request_threaded_irq(dev, client->irq, NULL, sec_nfc_irq_thread_fn,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					SEC_NFC_DRIVER_NAME, info);
	if (ret < 0) {
		NFC_LOG_ERR("probe() failed to register IRQ handler\n");
		return ret;
	}

	/* the rail is shared (also an I2C pull-up supply): one vote, dropped on unbind */
	if (pdata->pvdd && pdata->nfc_pvdd &&
	    !strstr(saved_command_line, "androidboot.mode=charger")) {
		if (!sec_nfc_regulator_onoff(pdata, NFC_I2C_LDO_ON)) {
			ret = devm_add_action_or_reset(dev, sec_nfc_pvdd_off, pdata);
			if (ret)
				return ret;
		}
		usleep_range(1000, 1100);
	}

	info->miscdev.minor = MISC_DYNAMIC_MINOR;
	info->miscdev.name = SEC_NFC_DRIVER_NAME;
	info->miscdev.fops = &sec_nfc_fops;
	info->miscdev.parent = dev;
	ret = misc_register(&info->miscdev);
	if (ret < 0) {
		NFC_LOG_ERR("probe() failed to register Device\n");
		return ret;
	}

	NFC_LOG_INFO("probe() success\n");

	return 0;
}

static void sec_nfc_remove(struct i2c_client *client)
{
	struct sec_nfc_info *info = i2c_get_clientdata(client);

	NFC_LOG_DBG("remove\n");

	misc_deregister(&info->miscdev);
	mutex_lock(&info->mutex);
	sec_nfc_set_mode(info, SEC_NFC_MODE_OFF);
	mutex_unlock(&info->mutex);
	gpiod_set_value_cansleep(info->pdata->firm, 0);
}

static const struct i2c_device_id sec_nfc_id_table[] = {
	{ SEC_NFC_DRIVER_NAME },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sec_nfc_id_table);

static const struct of_device_id nfc_match_table[] = {
	{ .compatible = "samsung,sec-nfc" },
	{ }
};
MODULE_DEVICE_TABLE(of, nfc_match_table);

static struct i2c_driver sec_nfc_driver = {
	.probe = sec_nfc_probe,
	.id_table = sec_nfc_id_table,
	.remove = sec_nfc_remove,
	.driver = {
		.name = SEC_NFC_DRIVER_NAME,
		.pm = pm_sleep_ptr(&sec_nfc_pm_ops),
		.of_match_table = nfc_match_table,
		.suppress_bind_attrs = true,
	},
};
module_i2c_driver(sec_nfc_driver);

MODULE_DESCRIPTION("Samsung sec_nfc driver");
MODULE_LICENSE("GPL");
