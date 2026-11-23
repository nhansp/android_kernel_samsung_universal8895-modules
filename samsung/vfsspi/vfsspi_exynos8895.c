// SPDX-License-Identifier: GPL-2.0-only
/*
 * Synaptics VFS8xxx fingerprint sensor on a secure-world SPI controller (Exynos 8895):
 * sensor power, the SPI clocks, the DRDY interrupt and the EL3 suspend/resume calls.
 */

#include <linux/build_bug.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/sched/signal.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <soc/samsung/exynos8895-smc.h>
#include <soc/samsung/secos_booster.h>

/* SET_CLK always selects MAX_BAUD_RATE on Exynos 8895 */
#define SLOW_BAUD_RATE		13000000
#define MAX_BAUD_RATE		13000000

#define VFSSPI_IOCTL_MAGIC			'k'
#define VFSSPI_IOCTL_DEVICE_RESET		_IO(VFSSPI_IOCTL_MAGIC, 2)
#define VFSSPI_IOCTL_SET_CLK			_IOW(VFSSPI_IOCTL_MAGIC, 3, unsigned int)
#define VFSSPI_IOCTL_REGISTER_DRDY_SIGNAL	_IOW(VFSSPI_IOCTL_MAGIC, 5, unsigned int)
#define VFSSPI_IOCTL_SET_DRDY_INT		_IOW(VFSSPI_IOCTL_MAGIC, 8, unsigned int)
#define VFSSPI_IOCTL_DEVICE_SUSPEND		_IO(VFSSPI_IOCTL_MAGIC, 9)
#define VFSSPI_IOCTL_POWER_ON			_IO(VFSSPI_IOCTL_MAGIC, 13)
#define VFSSPI_IOCTL_POWER_OFF			_IO(VFSSPI_IOCTL_MAGIC, 14)
#define VFSSPI_IOCTL_DISABLE_SPI_CLOCK		_IO(VFSSPI_IOCTL_MAGIC, 15)
#define VFSSPI_IOCTL_SET_SPI_CONFIGURATION	_IO(VFSSPI_IOCTL_MAGIC, 16)
#define VFSSPI_IOCTL_RESET_SPI_CONFIGURATION	_IO(VFSSPI_IOCTL_MAGIC, 17)
#define VFSSPI_IOCTL_GET_SENSOR_ORIENT		_IOR(VFSSPI_IOCTL_MAGIC, 18, unsigned int)
#define VFSSPI_IOCTL_CPU_SPEEDUP		_IOW(VFSSPI_IOCTL_MAGIC, 19, unsigned int)
#define VFSSPI_IOCTL_SET_SENSOR_TYPE		_IOW(VFSSPI_IOCTL_MAGIC, 20, unsigned int)
#define VFSSPI_IOCTL_SET_LOCKSCREEN		_IOW(VFSSPI_IOCTL_MAGIC, 22, unsigned int)
#define VFSSPI_IOCTL_POWER_CONTROL		_IOW(VFSSPI_IOCTL_MAGIC, 23, unsigned int)

/* the values the vendor fingerprint HAL (libbauthserver.so) issues */
static_assert(VFSSPI_IOCTL_DEVICE_RESET == 0x00006b02);
static_assert(VFSSPI_IOCTL_SET_CLK == 0x40046b03);
static_assert(VFSSPI_IOCTL_REGISTER_DRDY_SIGNAL == 0x40046b05);
static_assert(VFSSPI_IOCTL_SET_DRDY_INT == 0x40046b08);
static_assert(VFSSPI_IOCTL_DEVICE_SUSPEND == 0x00006b09);
static_assert(VFSSPI_IOCTL_POWER_ON == 0x00006b0d);
static_assert(VFSSPI_IOCTL_POWER_OFF == 0x00006b0e);
static_assert(VFSSPI_IOCTL_DISABLE_SPI_CLOCK == 0x00006b0f);
static_assert(VFSSPI_IOCTL_SET_SPI_CONFIGURATION == 0x00006b10);
static_assert(VFSSPI_IOCTL_RESET_SPI_CONFIGURATION == 0x00006b11);
static_assert(VFSSPI_IOCTL_GET_SENSOR_ORIENT == 0x80046b12);
static_assert(VFSSPI_IOCTL_CPU_SPEEDUP == 0x40046b13);
static_assert(VFSSPI_IOCTL_SET_SENSOR_TYPE == 0x40046b14);
static_assert(VFSSPI_IOCTL_SET_LOCKSCREEN == 0x40046b16);
static_assert(VFSSPI_IOCTL_POWER_CONTROL == 0x40046b17);

struct vfsspi_ioctl_register_signal {
	int user_pid;
	int signal_id;
};

#define SENSOR_OOO		(-2)
#define SENSOR_UNKNOWN		(-1)
#define SENSOR_FAILED		0
#define SENSOR_MAXIMUM		7

#define DETECT_ADM		1

#define VFSSPI_BOOST_TRIES	7

/* Lock order: lock -> irq_lock. The DRDY handler is threaded and takes irq_lock. */
struct vfsspi {
	struct device *dev;
	struct miscdevice misc;
	struct gpio_desc *drdy;
	struct gpio_desc *ldo;
	struct clk *pclk;
	struct clk *sclk_gate;
	struct clk *sclk;
	struct clk *qch;
	int irq;
	struct mutex lock;	/* open, release, ioctls */
	struct mutex irq_lock;	/* DRDY IRQ state, signal target */

	/* under lock */
	bool is_opened;
	bool ldo_on;
	bool enabled_clk;
	bool lockscreen;

	/* under irq_lock */
	bool irq_enabled;
	bool irq_wake;
	struct pid *pid;
	int signal_id;

	/* read by sysfs without a lock */
	int sensortype;
	unsigned int current_spi_speed;

	bool pm_suspended;
	u32 orient;
	u32 detect_mode;
	const char *chip_id;
};

static void vfsspi_power(struct vfsspi *fp, bool on)
{
	if (on) {
		gpiod_set_value_cansleep(fp->ldo, 1);
	} else {
		/* EL3 drives the sensor's chip select */
		long ret = exynos8895_smc(MC_FC_FP_CS_SET, 0, 0, 0);

		dev_dbg(fp->dev, "cs_set smc ret %ld\n", ret);
		gpiod_set_value_cansleep(fp->ldo, 0);
	}
	fp->ldo_on = on;
}

static void vfsspi_send_drdy_signal(struct vfsspi *fp)
{
	int ret;

	if (!fp->pid) {
		dev_dbg(fp->dev, "no DRDY signal target registered\n");
		return;
	}
	ret = kill_pid(fp->pid, fp->signal_id, 1);
	if (ret < 0)
		dev_err(fp->dev, "DRDY signal failed (%d)\n", ret);
}

static void vfsspi_irq_on(struct vfsspi *fp)
{
	if (fp->irq_enabled)
		return;
	pinctrl_pm_select_idle_state(fp->dev);
	enable_irq(fp->irq);
	if (fp->detect_mode == DETECT_ADM)
		fp->irq_wake = !enable_irq_wake(fp->irq);
	fp->irq_enabled = true;
}

static void vfsspi_irq_off(struct vfsspi *fp)
{
	if (!fp->irq_enabled)
		return;
	disable_irq_nosync(fp->irq);
	if (fp->irq_wake) {
		disable_irq_wake(fp->irq);
		fp->irq_wake = false;
	}
	fp->irq_enabled = false;
	pinctrl_pm_select_sleep_state(fp->dev);
}

static irqreturn_t vfsspi_drdy_irq(int irq, void *data)
{
	struct vfsspi *fp = data;

	/* an edge replayed by enable_irq() can find DRDY low again */
	if (gpiod_get_value_cansleep(fp->drdy) != 1)
		return IRQ_HANDLED;

	mutex_lock(&fp->irq_lock);
	if (fp->irq_enabled) {
		vfsspi_irq_off(fp);
		vfsspi_send_drdy_signal(fp);
		pm_wakeup_event(fp->dev, 3000);
	}
	mutex_unlock(&fp->irq_lock);

	return IRQ_HANDLED;
}

static int vfsspi_set_clk(struct vfsspi *fp, void __user *argp)
{
	unsigned short clock;
	int ret;

	if (copy_from_user(&clock, argp, sizeof(clock)))
		return -EFAULT;

	/* 0 (running), 0xFFFF (slow) and any other value all give 13 MHz */
	fp->current_spi_speed = MAX_BAUD_RATE;
	if (fp->enabled_clk)
		return 0;

	ret = clk_prepare_enable(fp->qch);
	if (ret)
		return ret;
	ret = clk_prepare_enable(fp->pclk);
	if (ret)
		goto err_qch;
	ret = clk_prepare_enable(fp->sclk_gate);
	if (ret)
		goto err_pclk;
	ret = clk_set_rate(fp->sclk, fp->current_spi_speed * 2);
	if (ret)
		goto err_sclk;

	pm_stay_awake(fp->dev);
	fp->enabled_clk = true;
	dev_dbg(fp->dev, "ENABLE_SPI_CLOCK %u\n", fp->current_spi_speed);
	return 0;

err_sclk:
	clk_disable_unprepare(fp->sclk_gate);
err_pclk:
	clk_disable_unprepare(fp->pclk);
err_qch:
	clk_disable_unprepare(fp->qch);
	dev_err(fp->dev, "unable to enable spi clk (%d)\n", ret);
	return ret;
}

/* in the vendor driver's order: the bus clock and its Q-channel, then the functional clock */
static void vfsspi_disable_spi_clock(struct vfsspi *fp)
{
	if (!fp->enabled_clk)
		return;
	clk_disable_unprepare(fp->qch);
	clk_disable_unprepare(fp->pclk);
	clk_disable_unprepare(fp->sclk_gate);
	pm_relax(fp->dev);
	fp->enabled_clk = false;
}

static int vfsspi_register_drdy_signal(struct vfsspi *fp, void __user *argp)
{
	struct vfsspi_ioctl_register_signal sig;
	struct pid *pid, *old;

	if (copy_from_user(&sig, argp, sizeof(sig)))
		return -EFAULT;

	pid = find_get_pid(sig.user_pid);
	mutex_lock(&fp->irq_lock);
	old = fp->pid;
	fp->pid = pid;
	fp->signal_id = sig.signal_id;
	mutex_unlock(&fp->irq_lock);
	put_pid(old);

	return pid ? 0 : -ENODEV;
}

static int vfsspi_set_drdy_int(struct vfsspi *fp, void __user *argp)
{
	unsigned short enable;

	if (copy_from_user(&enable, argp, sizeof(enable)))
		return -EFAULT;

	mutex_lock(&fp->irq_lock);
	if (!enable) {
		vfsspi_irq_off(fp);
	} else {
		vfsspi_irq_on(fp);
		/* DRDY asserted before the IRQ was enabled: signal now */
		if (gpiod_get_value_cansleep(fp->drdy) == 1)
			vfsspi_send_drdy_signal(fp);
	}
	mutex_unlock(&fp->irq_lock);

	return 0;
}

static int vfsspi_cpu_speedup(struct vfsspi *fp, void __user *argp)
{
	unsigned int onoff;
	int tries = 0;
	int ret;

	if (copy_from_user(&onoff, argp, sizeof(onoff)))
		return -EFAULT;

	if (!onoff) {
		ret = secos_booster_stop();
		if (ret)
			dev_err(fp->dev, "booster stop failed. (%d)\n", ret);
		return ret;
	}

	do {
		ret = secos_booster_start((enum secos_boost_policy)(onoff - 1));
		tries++;
		if (ret) {
			dev_err(fp->dev, "booster start failed. (%d) retry: %d\n", ret, tries);
			if (tries < VFSSPI_BOOST_TRIES)
				usleep_range(500, 510);
		}
	} while (ret && tries < VFSSPI_BOOST_TRIES);

	return ret;
}

static int vfsspi_set_sensor_type(struct vfsspi *fp, void __user *argp)
{
	unsigned int type;
	int t;

	if (copy_from_user(&type, argp, sizeof(type)))
		return -EFAULT;

	t = (int)type;
	if (t >= SENSOR_OOO && t < SENSOR_MAXIMUM) {
		/* an out-of-order report keeps a failed type */
		if (!(t == SENSOR_OOO && fp->sensortype == SENSOR_FAILED))
			WRITE_ONCE(fp->sensortype, t);
	} else {
		dev_err(fp->dev, "SET_SENSOR_TYPE: invalid value %d\n", t);
		WRITE_ONCE(fp->sensortype, SENSOR_UNKNOWN);
	}
	return 0;
}

static long vfsspi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vfsspi *fp = container_of(file->private_data, struct vfsspi, misc);
	void __user *argp = (void __user *)arg;
	unsigned int val;
	long ret = 0;

	if (_IOC_TYPE(cmd) != VFSSPI_IOCTL_MAGIC)
		return -ENOTTY;

	mutex_lock(&fp->lock);
	switch (cmd) {
	/* the vendor driver only logged these */
	case VFSSPI_IOCTL_DEVICE_RESET:
	case VFSSPI_IOCTL_DEVICE_SUSPEND:
	case VFSSPI_IOCTL_SET_SPI_CONFIGURATION:
	case VFSSPI_IOCTL_RESET_SPI_CONFIGURATION:
		break;
	case VFSSPI_IOCTL_SET_CLK:
		ret = vfsspi_set_clk(fp, argp);
		break;
	case VFSSPI_IOCTL_REGISTER_DRDY_SIGNAL:
		ret = vfsspi_register_drdy_signal(fp, argp);
		break;
	case VFSSPI_IOCTL_SET_DRDY_INT:
		ret = vfsspi_set_drdy_int(fp, argp);
		break;
	case VFSSPI_IOCTL_POWER_ON:
		if (!fp->ldo_on)
			vfsspi_power(fp, true);
		break;
	case VFSSPI_IOCTL_POWER_OFF:
		if (fp->ldo_on)
			vfsspi_power(fp, false);
		break;
	case VFSSPI_IOCTL_POWER_CONTROL:	/* unconditional, unlike POWER_ON/OFF */
		if (copy_from_user(&val, argp, sizeof(val))) {
			ret = -EFAULT;
			break;
		}
		vfsspi_power(fp, val);
		break;
	case VFSSPI_IOCTL_DISABLE_SPI_CLOCK:
		vfsspi_disable_spi_clock(fp);
		break;
	case VFSSPI_IOCTL_CPU_SPEEDUP:
		ret = vfsspi_cpu_speedup(fp, argp);
		break;
	case VFSSPI_IOCTL_SET_SENSOR_TYPE:
		ret = vfsspi_set_sensor_type(fp, argp);
		break;
	case VFSSPI_IOCTL_SET_LOCKSCREEN:
		if (copy_from_user(&val, argp, sizeof(val))) {
			ret = -EFAULT;
			break;
		}
		fp->lockscreen = val;
		break;
	case VFSSPI_IOCTL_GET_SENSOR_ORIENT:
		if (put_user(fp->orient, (unsigned int __user *)argp))
			ret = -EFAULT;
		break;
	default:
		/* the vendor driver returned -EFAULT for any other command */
		dev_info(fp->dev, "default error. %u\n", cmd);
		ret = -EFAULT;
		break;
	}
	mutex_unlock(&fp->lock);

	return ret;
}

static int vfsspi_open(struct inode *inode, struct file *file)
{
	struct vfsspi *fp = container_of(file->private_data, struct vfsspi, misc);
	int ret = 0;

	mutex_lock(&fp->lock);
	if (fp->is_opened)
		ret = -EBUSY;
	else
		fp->is_opened = true;
	mutex_unlock(&fp->lock);
	if (ret)
		return ret;

	return nonseekable_open(inode, file);
}

static int vfsspi_release(struct inode *inode, struct file *file)
{
	struct vfsspi *fp = container_of(file->private_data, struct vfsspi, misc);

	mutex_lock(&fp->lock);
	fp->is_opened = false;
	if (fp->ldo_on)
		vfsspi_power(fp, false);
	mutex_unlock(&fp->lock);

	return 0;
}

/* the vendor driver's secure build did nothing on read and write */
static ssize_t vfsspi_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	return 0;
}

static ssize_t vfsspi_write(struct file *file, const char __user *buf, size_t count,
			    loff_t *ppos)
{
	return 0;
}

static const struct file_operations vfsspi_fops = {
	.owner		= THIS_MODULE,
	.read		= vfsspi_read,
	.write		= vfsspi_write,
	.unlocked_ioctl	= vfsspi_ioctl,
	.open		= vfsspi_open,
	.release	= vfsspi_release,
};

static const struct class vfsspi_fp_class = {
	.name = "fingerprint",
};

static ssize_t bfs_values_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct vfsspi *fp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "\"FP_SPICLK\":\"%u\"\n", READ_ONCE(fp->current_spi_speed));
}
static DEVICE_ATTR_RO(bfs_values);

static ssize_t type_check_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct vfsspi *fp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(fp->sensortype));
}
static DEVICE_ATTR_RO(type_check);

static ssize_t vendor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", "SYNAPTICS");
}
static DEVICE_ATTR_RO(vendor);

static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct vfsspi *fp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", fp->chip_id);
}
static DEVICE_ATTR_RO(name);

static ssize_t adm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct vfsspi *fp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", fp->detect_mode);
}
static DEVICE_ATTR_RO(adm);

static struct attribute *vfsspi_fp_attrs[] = {
	&dev_attr_bfs_values.attr,
	&dev_attr_type_check.attr,
	&dev_attr_vendor.attr,
	&dev_attr_name.attr,
	&dev_attr_adm.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vfsspi_fp);

static void vfsspi_class_unregister(void *data)
{
	class_unregister(&vfsspi_fp_class);
}

static void vfsspi_device_unregister(void *data)
{
	device_unregister(data);
}

static void vfsspi_misc_deregister(void *data)
{
	misc_deregister(data);
}

static void vfsspi_teardown(void *data)
{
	struct vfsspi *fp = data;

	mutex_lock(&fp->lock);
	mutex_lock(&fp->irq_lock);
	vfsspi_irq_off(fp);
	put_pid(fp->pid);
	fp->pid = NULL;
	mutex_unlock(&fp->irq_lock);
	vfsspi_disable_spi_clock(fp);
	if (fp->ldo_on)
		vfsspi_power(fp, false);
	mutex_unlock(&fp->lock);
}

static int vfsspi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vfsspi *fp;
	struct device *fp_dev;
	int ret;

	fp = devm_kzalloc(dev, sizeof(*fp), GFP_KERNEL);
	if (!fp)
		return -ENOMEM;
	fp->dev = dev;
	fp->current_spi_speed = SLOW_BAUD_RATE;
	fp->sensortype = SENSOR_UNKNOWN;
	platform_set_drvdata(pdev, fp);

	if (device_property_read_u32(dev, "samsung,orient", &fp->orient))
		fp->orient = 0;
	if (device_property_read_u32(dev, "samsung,detect-mode", &fp->detect_mode))
		fp->detect_mode = DETECT_ADM;
	if (device_property_read_string(dev, "samsung,chip-id", &fp->chip_id))
		fp->chip_id = "";

	fp->drdy = devm_gpiod_get(dev, "drdy", GPIOD_IN);
	if (IS_ERR(fp->drdy))
		return dev_err_probe(dev, PTR_ERR(fp->drdy), "drdy gpio\n");
	fp->ldo = devm_gpiod_get(dev, "ldo", GPIOD_OUT_LOW);
	if (IS_ERR(fp->ldo))
		return dev_err_probe(dev, PTR_ERR(fp->ldo), "ldo gpio\n");

	fp->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(fp->pclk))
		return dev_err_probe(dev, PTR_ERR(fp->pclk), "pclk\n");
	fp->sclk_gate = devm_clk_get(dev, "sclk_gate");
	if (IS_ERR(fp->sclk_gate))
		return dev_err_probe(dev, PTR_ERR(fp->sclk_gate), "sclk_gate\n");
	fp->sclk = devm_clk_get(dev, "sclk");
	if (IS_ERR(fp->sclk))
		return dev_err_probe(dev, PTR_ERR(fp->sclk), "sclk\n");
	fp->qch = devm_clk_get_optional(dev, "qch");
	if (IS_ERR(fp->qch))
		return dev_err_probe(dev, PTR_ERR(fp->qch), "qch\n");

	ret = devm_mutex_init(dev, &fp->lock);
	if (ret)
		return ret;
	ret = devm_mutex_init(dev, &fp->irq_lock);
	if (ret)
		return ret;
	ret = devm_device_init_wakeup(dev);
	if (ret)
		return ret;

	fp->irq = platform_get_irq(pdev, 0);
	if (fp->irq < 0)
		return fp->irq;
	ret = devm_request_threaded_irq(dev, fp->irq, NULL, vfsspi_drdy_irq,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"vfsspi_irq", fp);
	if (ret)
		return dev_err_probe(dev, ret, "drdy irq\n");
	ret = devm_add_action_or_reset(dev, vfsspi_teardown, fp);
	if (ret)
		return ret;
	pinctrl_pm_select_sleep_state(dev);

	ret = class_register(&vfsspi_fp_class);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, vfsspi_class_unregister, NULL);
	if (ret)
		return ret;
	fp_dev = device_create_with_groups(&vfsspi_fp_class, NULL, MKDEV(0, 0), fp,
					   vfsspi_fp_groups, "fingerprint");
	if (IS_ERR(fp_dev))
		return PTR_ERR(fp_dev);
	ret = devm_add_action_or_reset(dev, vfsspi_device_unregister, fp_dev);
	if (ret)
		return ret;

	/* the HAL opens /dev/vfsspi by path only */
	fp->misc.minor = MISC_DYNAMIC_MINOR;
	fp->misc.name = "vfsspi";
	fp->misc.fops = &vfsspi_fops;
	fp->misc.parent = dev;
	ret = misc_register(&fp->misc);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, vfsspi_misc_deregister, &fp->misc);
	if (ret)
		return ret;

	dev_dbg(dev, "ready, drdy irq %d\n", fp->irq);
	return 0;
}

/*
 * EL3 expects the resume SMC before the pin states are restored (the vendor kernel issued
 * it from the Samsung pinctrl resume); Samsung pinctrl restores them at resume_early.
 */
static int vfsspi_suspend(struct device *dev)
{
	struct vfsspi *fp = dev_get_drvdata(dev);
	unsigned long id;
	long ret;

	mutex_lock(&fp->irq_lock);
	id = fp->detect_mode && fp->irq_enabled ? MC_FC_FP_PM_SUSPEND_RETAIN : MC_FC_FP_PM_SUSPEND;
	mutex_unlock(&fp->irq_lock);

	fp->pm_suspended = true;
	ret = exynos8895_smc(id, 0, 0, 0);
	dev_dbg(dev, "suspend smc 0x%lx ret %ld\n", id, ret);

	return 0;
}

static int vfsspi_resume_noirq(struct device *dev)
{
	struct vfsspi *fp = dev_get_drvdata(dev);
	long ret;

	if (!fp->pm_suspended)
		return 0;
	fp->pm_suspended = false;
	ret = exynos8895_smc(MC_FC_FP_PM_RESUME, 0, 0, 0);
	dev_dbg(dev, "resume smc 0x%x ret %ld\n", MC_FC_FP_PM_RESUME, ret);

	return 0;
}

static const struct dev_pm_ops vfsspi_pm_ops = {
	.suspend	= pm_sleep_ptr(vfsspi_suspend),
	.resume_noirq	= pm_sleep_ptr(vfsspi_resume_noirq),
};

static const struct of_device_id vfsspi_of_match[] = {
	{ .compatible = "samsung,exynos8895-vfsspi" },
	{ }
};
MODULE_DEVICE_TABLE(of, vfsspi_of_match);

static struct platform_driver vfsspi_driver = {
	.probe = vfsspi_probe,
	.driver = {
		.name = "vfsspi-exynos8895",
		.of_match_table = vfsspi_of_match,
		.pm = pm_sleep_ptr(&vfsspi_pm_ops),
		/* an open /dev/vfsspi would outlive an unbind */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(vfsspi_driver);

MODULE_DESCRIPTION("Synaptics VFS8xxx fingerprint sensor on Exynos 8895, non-secure side");
MODULE_LICENSE("GPL");
