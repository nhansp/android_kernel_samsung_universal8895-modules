// SPDX-License-Identifier: GPL-2.0
/*
 * Broadcom BCM43xx Bluetooth power and low power mode control for the
 * Samsung Exynos UART, after Samsung's drivers/bluetooth/bcm43xx.c.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/rcupdate.h>
#include <linux/rfkill.h>
#include <linux/serial_core.h>
#include <linux/serial_s3c.h>
#include <linux/slab.h>

#define BT_POWER_ON_SETTLE_MS	100
#define BT_ENTER_LPM_DELAY	ktime_set(1, 0)
#define BT_BTWAKE_RELAX_MS	500
#define BT_HOSTWAKE_RELAX_MS	1000

/*
 * 1: BT_WAKE follows UART transmit and drops 1 s after the last byte.
 * 0: BT_WAKE stays high while powered, so the controller never sleeps.
 */
static int bt_lpm_mode = 1;
module_param_named(lpm, bt_lpm_mode, int, 0644);

struct bt_lpm {
	struct device *dev;
	struct device *uart;
	struct gpio_desc *en;
	struct gpio_desc *wake;
	struct gpio_desc *hw;
	int irq;
	struct rfkill *rfk;
	struct hrtimer timer;
	struct wakeup_source *ws_bt;
	struct wakeup_source *ws_host;
	bool host_wake;
	bool dev_wake;
	bool powered;
};

/* The UART's wake_peer hook gets only the port */
static struct bt_lpm *bt_lpm_g;

static int bt_set_block(void *data, bool blocked)
{
	struct bt_lpm *bl = data;

	if (!blocked) {
		/* rfkill calls set_block for every state write, also an unchanged one */
		if (bl->powered)
			return 0;
		enable_irq_wake(bl->irq);
		if (!bt_lpm_mode)
			gpiod_set_value(bl->wake, 1);
		gpiod_set_value(bl->en, 1);
		msleep(BT_POWER_ON_SETTLE_MS);
		WRITE_ONCE(bl->powered, true);
		dev_dbg(bl->dev, "power on\n");
	} else {
		if (!bl->powered) {
			gpiod_set_value(bl->en, 0);
			return 0;
		}
		WRITE_ONCE(bl->powered, false);
		disable_irq_wake(bl->irq);
		gpiod_set_value(bl->en, 0);
		hrtimer_cancel(&bl->timer);
		gpiod_set_value(bl->wake, 0);
		bl->dev_wake = false;
		/* Samsung's driver left the timer running here; cancelled, it must relax ws_bt */
		__pm_relax(bl->ws_bt);
		bl->host_wake = false;
		__pm_relax(bl->ws_host);
		dev_dbg(bl->dev, "power off\n");
	}
	return 0;
}

static const struct rfkill_ops bt_rfkill_ops = {
	.set_block = bt_set_block,
};

/* Called under the UART port lock */
static void bt_wake_peer(struct uart_port *port)
{
	struct bt_lpm *bl = READ_ONCE(bt_lpm_g);

	if (!bl || !bt_lpm_mode || !READ_ONCE(bl->powered))
		return;
	/*
	 * A call racing with power off can re-arm the timer after the off path cancelled it;
	 * the timer then drops BT_WAKE and ws_bt 1 s later.
	 */
	hrtimer_try_to_cancel(&bl->timer);
	__pm_stay_awake(bl->ws_bt);
	gpiod_set_value(bl->wake, 1);
	bl->dev_wake = true;
	hrtimer_start(&bl->timer, BT_ENTER_LPM_DELAY, HRTIMER_MODE_REL);
}

static enum hrtimer_restart bt_enter_lpm(struct hrtimer *t)
{
	struct bt_lpm *bl = container_of(t, struct bt_lpm, timer);

	gpiod_set_value(bl->wake, 0);
	bl->dev_wake = false;
	__pm_wakeup_event(bl->ws_bt, BT_BTWAKE_RELAX_MS);
	return HRTIMER_NORESTART;
}

static irqreturn_t bt_host_wake_thread(int irq, void *data)
{
	struct bt_lpm *bl = data;
	/* The Exynos EINT irqchip cannot report the line level */
	bool v = gpiod_get_value(bl->hw);

	if (v == bl->host_wake)
		return IRQ_HANDLED;
	bl->host_wake = v;
	if (v)
		__pm_stay_awake(bl->ws_host);
	else
		__pm_wakeup_event(bl->ws_host, BT_HOSTWAKE_RELAX_MS);
	return IRQ_HANDLED;
}

static void bt_lpm_ws_unregister(void *ws)
{
	wakeup_source_unregister(ws);
}

static int bcm43xx_bt_lpm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *un;
	struct platform_device *uart;
	struct bt_lpm *bl;
	int ret;

	bl = devm_kzalloc(dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;
	bl->dev = dev;

	un = of_parse_phandle(np, "samsung,uart", 0);
	uart = un ? of_find_device_by_node(un) : NULL;
	of_node_put(un);
	if (!uart)
		return -EPROBE_DEFER;
	bl->uart = &uart->dev;

	/* First, so that a deferral (UART not bound yet) leaves nothing to undo */
	ret = s3c24xx_serial_set_wake_peer(bl->uart, bt_wake_peer);
	if (ret) {
		put_device(bl->uart);
		return ret;
	}

	bl->en = devm_gpiod_get(dev, "bt-en", GPIOD_OUT_LOW);
	if (IS_ERR(bl->en)) {
		ret = PTR_ERR(bl->en);
		goto err_peer;
	}

	bl->wake = devm_gpiod_get(dev, "bt-wake", GPIOD_OUT_LOW);
	if (IS_ERR(bl->wake)) {
		ret = PTR_ERR(bl->wake);
		goto err_peer;
	}

	/* Before the IRQ request, which switches the pin from input to EINT */
	bl->hw = devm_gpiod_get(dev, "bt-host-wake", GPIOD_IN);
	if (IS_ERR(bl->hw)) {
		ret = PTR_ERR(bl->hw);
		goto err_peer;
	}

	hrtimer_setup(&bl->timer, bt_enter_lpm, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	/* Registered before the IRQ, so that devm frees the IRQ first */
	bl->ws_host = wakeup_source_register(dev, "BT_host_wake");
	if (!bl->ws_host) {
		ret = -ENOMEM;
		goto err_peer;
	}
	ret = devm_add_action_or_reset(dev, bt_lpm_ws_unregister, bl->ws_host);
	if (ret)
		goto err_peer;

	bl->ws_bt = wakeup_source_register(dev, "BT_bt_wake");
	if (!bl->ws_bt) {
		ret = -ENOMEM;
		goto err_peer;
	}
	ret = devm_add_action_or_reset(dev, bt_lpm_ws_unregister, bl->ws_bt);
	if (ret)
		goto err_peer;

	bl->irq = platform_get_irq(pdev, 0);
	if (bl->irq < 0) {
		ret = bl->irq;
		goto err_peer;
	}

	ret = devm_request_threaded_irq(dev, bl->irq, NULL, bt_host_wake_thread,
					IRQF_ONESHOT, "bt_host_wake", bl);
	if (ret)
		goto err_peer;

	bl->rfk = rfkill_alloc("bcm43xx Bluetooth", dev, RFKILL_TYPE_BLUETOOTH,
			       &bt_rfkill_ops, bl);
	if (!bl->rfk) {
		ret = -ENOMEM;
		goto err_peer;
	}

	rfkill_init_sw_state(bl->rfk, 0);
	ret = rfkill_register(bl->rfk);
	if (ret) {
		rfkill_destroy(bl->rfk);
		goto err_peer;
	}

	rfkill_set_sw_state(bl->rfk, true);

	platform_set_drvdata(pdev, bl);
	WRITE_ONCE(bt_lpm_g, bl);

	return 0;

err_peer:
	s3c24xx_serial_set_wake_peer(bl->uart, NULL);
	put_device(bl->uart);
	return ret;
}

static void bcm43xx_bt_lpm_remove(struct platform_device *pdev)
{
	struct bt_lpm *bl = platform_get_drvdata(pdev);

	rfkill_unregister(bl->rfk);
	rfkill_destroy(bl->rfk);

	WRITE_ONCE(bt_lpm_g, NULL);
	s3c24xx_serial_set_wake_peer(bl->uart, NULL);
	/* bt_wake_peer() runs with the port lock held and interrupts off */
	synchronize_rcu();

	bt_set_block(bl, true);
	hrtimer_cancel(&bl->timer);
	put_device(bl->uart);
}

static const struct of_device_id bcm43xx_bt_lpm_of_match[] = {
	{ .compatible = "samsung,bcm43xx" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, bcm43xx_bt_lpm_of_match);

static struct platform_driver bcm43xx_bt_lpm_driver = {
	.probe = bcm43xx_bt_lpm_probe,
	.remove = bcm43xx_bt_lpm_remove,
	.driver = {
		.name = "bcm43xx-bt-lpm",
		.of_match_table = bcm43xx_bt_lpm_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(bcm43xx_bt_lpm_driver);

MODULE_DESCRIPTION("Broadcom BCM43xx Bluetooth LPM Driver");
MODULE_LICENSE("GPL");
