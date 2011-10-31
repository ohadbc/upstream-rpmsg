/*
 * Remote processor machine-specific module for Davinci
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)    "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/printk.h>
#include <linux/bitops.h>
#include <linux/platform_device.h>
#include <linux/amp/remoteproc.h>
#include <linux/clk.h>
#include <linux/io.h>

#include <mach/da8xx.h>
#include <mach/cputype.h>
#include <mach/psc.h>
#include <mach/remoteproc.h>

/*
 * Technical Reference:
 * OMAP-L138 Applications Processor System Reference Guide
 * http://www.ti.com/litv/pdf/sprugm7d
 */

/* local reset bit (0 is asserted) in MDCTL15 register (section 9.6.18) */
#define LRST		BIT(8)

/* next state bits in MDCTL15 register (section 9.6.18) */
#define NEXT_ENABLED	0x3

/* register for DSP boot address in SYSCFG0 module (section 11.5.6) */
#define HOST1CFG	0x44

static inline int davinci_rproc_start(struct rproc *rproc)
{
	struct device *dev = rproc->dev;
	struct davinci_rproc_pdata *pdata = dev->platform_data;
	struct davinci_soc_info *soc_info = &davinci_soc_info;
	u64 bootaddr = rproc->bootaddr;
	void __iomem *psc_base;
	struct clk *dsp_clk;

	/* hw requires the start (boot) address be on 1KB boundary */
	if (bootaddr & 0x3ff) {
		dev_err(dev, "invalid boot address: must be aligned to 1KB\n");
		return -EINVAL;
	}

	dsp_clk = clk_get(dev, pdata->clk_name);
	if (IS_ERR_OR_NULL(dsp_clk)) {
		dev_err(dev, "clk_get error: %ld\n", PTR_ERR(dsp_clk));
		return PTR_ERR(dsp_clk);
	}

	clk_enable(dsp_clk);
	rproc->priv = dsp_clk;

	psc_base = ioremap(soc_info->psc_bases[0], SZ_4K);

	/* insure local reset is asserted before writing start address */
	__raw_writel(NEXT_ENABLED, psc_base + MDCTL + 4 * DA8XX_LPSC0_GEM);

	__raw_writel(bootaddr, DA8XX_SYSCFG0_VIRT(HOST1CFG));

	/* de-assert local reset to start the dsp running */
	__raw_writel(LRST | NEXT_ENABLED, psc_base + MDCTL +
		4 * DA8XX_LPSC0_GEM);

	iounmap(psc_base);

	return 0;
}

static inline int davinci_rproc_stop(struct rproc *rproc)
{
	struct davinci_soc_info *soc_info = &davinci_soc_info;
	void __iomem *psc_base;
	struct clk *dsp_clk = rproc->priv;

	psc_base = ioremap(soc_info->psc_bases[0], SZ_4K);

	/* halt the dsp by asserting local reset */
	__raw_writel(NEXT_ENABLED, psc_base + MDCTL + 4 * DA8XX_LPSC0_GEM);

	clk_disable(dsp_clk);
	clk_put(dsp_clk);

	iounmap(psc_base);

	return 0;
}

static struct rproc_ops davinci_rproc_ops = {
	.start = davinci_rproc_start,
	.stop = davinci_rproc_stop,
};

static int davinci_rproc_probe(struct platform_device *pdev)
{
	struct davinci_rproc_pdata *pdata = pdev->dev.platform_data;

	return rproc_register(&pdev->dev, pdata->name, &davinci_rproc_ops,
				pdata->firmware, NULL, THIS_MODULE);
}

static int __devexit davinci_rproc_remove(struct platform_device *pdev)
{
	struct davinci_rproc_pdata *pdata = pdev->dev.platform_data;

	return rproc_unregister(pdata->name);
}

static struct platform_driver davinci_rproc_driver = {
	.probe = davinci_rproc_probe,
	.remove = __devexit_p(davinci_rproc_remove),
	.driver = {
		.name = "davinci-rproc",
		.owner = THIS_MODULE,
	},
};

static int __init davinci_rproc_init(void)
{
	return platform_driver_register(&davinci_rproc_driver);
}
module_init(davinci_rproc_init);

static void __exit davinci_rproc_exit(void)
{
	platform_driver_unregister(&davinci_rproc_driver);
}
module_exit(davinci_rproc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Davinci Remote Processor control driver");
