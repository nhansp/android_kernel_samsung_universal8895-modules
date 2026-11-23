// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos 8895 platform glue for the dhd driver: WLAN_REG_ON, the PCIe host power
 * and the preallocated buffers, from the "samsung,brcm-wlan" node.
 */

#include <linux/delay.h>
#include <linux/exynos-pci-ctrl.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/wlan_plat.h>

#include "dhd_custom_exynos8895.h"

#define WIFI_TURNON_DELAY_MS	200
#define WIFI_PCIE_CH		0

static struct gpio_desc *wlan_reg_on;

struct resource dhd_wlan_resources = {
	.name	= "bcmdhd_wlan_irq",
	.flags	= IORESOURCE_IRQ | IORESOURCE_IRQ_SHAREABLE |
		  IORESOURCE_IRQ_HIGHEDGE,
};

static int dhd_wlan_power(int onoff)
{
	pr_debug("%s: power %s\n", __func__, onoff ? "on" : "off");
	if (!onoff)
		exynos_pcie_pm_suspend(WIFI_PCIE_CH);
	gpiod_set_value(wlan_reg_on, onoff);
	if (onoff) {
		exynos_pcie_l1ss_ctrl(0, PCIE_L1SS_CTRL_WIFI);
		exynos_pcie_pm_resume(WIFI_PCIE_CH);
	}
	return 0;
}

static int dhd_wlan_reset(int onoff)
{
	return 0;
}

struct wifi_platform_data dhd_wlan_control = {
	.set_power	= dhd_wlan_power,
	.set_reset	= dhd_wlan_reset,
	.mem_prealloc	= dhd_wlan_mem_prealloc,
};

static int bcmdhd_exynos8895_probe(struct platform_device *pdev)
{
	struct device_node *pn = of_parse_phandle(pdev->dev.of_node,
						  "samsung,pcie", 0);
	struct platform_device *pp = pn ? of_find_device_by_node(pn) : NULL;
	bool host_ready;
	int irq, ret;

	of_node_put(pn);
	host_ready = pp && platform_get_drvdata(pp);
	if (pp)
		put_device(&pp->dev);
	if (!host_ready)
		return -EPROBE_DEFER;

	wlan_reg_on = devm_gpiod_get(&pdev->dev, "wlan-reg-on", GPIOD_OUT_HIGH);
	if (IS_ERR(wlan_reg_on))
		return PTR_ERR(wlan_reg_on);

	msleep(WIFI_TURNON_DELAY_MS);

	ret = exynos_pcie_pm_resume(WIFI_PCIE_CH);
	if (ret)
		dev_err(&pdev->dev, "first PCIe power-on failed: %d\n", ret);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	dhd_wlan_resources.start = irq;
	dhd_wlan_resources.end = irq;

	ret = dhd_init_wlan_mem();
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to alloc reserved memory, ret=%d\n", ret);
		/* Samsung's glue went on with the freed buffers; let dhd fail the requests */
		dhd_wlan_control.mem_prealloc = NULL;
	}

	return dhd_module_init();
}

static void bcmdhd_exynos8895_remove(struct platform_device *pdev)
{
	dhd_module_exit();
	if (dhd_wlan_control.mem_prealloc)
		dhd_exit_wlan_mem();
}

static const struct of_device_id bcmdhd_exynos8895_of[] = {
	{ .compatible = "samsung,brcm-wlan" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcmdhd_exynos8895_of);

static struct platform_driver bcmdhd_exynos8895_drv = {
	.probe	= bcmdhd_exynos8895_probe,
	.remove	= bcmdhd_exynos8895_remove,
	.driver	= {
		.name		= "bcmdhd-exynos8895",
		.of_match_table	= bcmdhd_exynos8895_of,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(bcmdhd_exynos8895_drv);
