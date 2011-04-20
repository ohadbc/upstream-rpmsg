/*
 * Remote processor machine-specific module for OMAP4
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
#include <linux/platform_device.h>
#include <linux/iommu.h>
#include <linux/slab.h>
#include <linux/remoteproc.h>

#include <plat/iommu.h>
#include <plat/omap_device.h>
#include <plat/remoteproc.h>

struct omap_rproc_priv {
	struct iommu_domain *domain;
	struct device *iommu;
};

/*
 * Map a physically contiguous memory region using biggest pages possible.
 * TODO: this code should go away; it belongs in the generic IOMMU layer
 */
static int omap_rproc_map_unmap(struct iommu_domain *domain,
				const struct rproc_mem_entry *me, bool map)
{
	u32 all_bits;
	/* these are the page sizes supported by OMAP's IOMMU */
	u32 pg_size[] = {SZ_16M, SZ_1M, SZ_64K, SZ_4K};
	int i, ret, size = me->size;
	u32 da = me->da;
	u32 pa = me->pa;
	int order;
	int flags;

	/* must be aligned at least with the smallest supported iommu page */
	if (!IS_ALIGNED(size, SZ_4K) || !IS_ALIGNED(da | pa, SZ_4K)) {
		pr_err("misaligned: size %x da 0x%x pa 0x%x\n", size, da, pa);
		return -EINVAL;
	}

	while (size) {
		/* find the max page size with which both pa, da are aligned */
		all_bits = pa | da;
		for (i = 0; i < ARRAY_SIZE(pg_size); i++) {
			if ((size >= pg_size[i]) &&
				((all_bits & (pg_size[i] - 1)) == 0)) {
				break;
			}
		}

		order = get_order(pg_size[i]);

		/* OMAP4's M3 is little endian, so no need for conversions */
		flags = MMU_RAM_ENDIAN_LITTLE | MMU_RAM_ELSZ_NONE;

		if (map)
			ret = iommu_map(domain, da, pa, order, flags);
		else
			ret = iommu_unmap(domain, da, order);

		if (ret)
			return ret;

		size -= pg_size[i];
		da += pg_size[i];
		pa += pg_size[i];
	}

	return 0;
}

/* bootaddr isn't needed for the dual M3's */
static inline int omap_rproc_start(struct rproc *rproc, u64 bootaddr)
{
	struct device *dev = rproc->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_rproc_pdata *pdata = dev->platform_data;
	struct iommu_domain *domain;
	struct device *iommu;
	struct omap_rproc_priv *priv;
	int ret, i;

	if (!iommu_found()) {
		dev_err(&pdev->dev, "iommu not found\n");
		return -ENODEV;
	}

	priv = kmalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		dev_err(&pdev->dev, "kzalloc failed\n");
		return -ENOMEM;
	}

	/*
	 * Use the specified iommu name to find our iommu device.
	 * TODO: solve this generically so other platforms can use it, too
	 */
	iommu = omap_find_iommu_device(pdata->iommu_name);
	if (!iommu) {
		dev_err(dev, "omap_find_iommu_device failed\n");
		ret = -ENODEV;
		goto free_mem;
	}

	domain = iommu_domain_alloc();
	if (!domain) {
		dev_err(&pdev->dev, "can't alloc iommu domain\n");
		ret = -ENODEV;
		goto free_mem;
	}

	priv->iommu = iommu;
	priv->domain = domain;
	rproc->priv = priv;

	ret = iommu_attach_device(domain, iommu);
	if (ret) {
		dev_err(&pdev->dev, "can't attach iommu device: %d\n", ret);
		goto free_domain;
	}

	for (i = 0; rproc->memory_maps[i].size; i++) {
		const struct rproc_mem_entry *me = &rproc->memory_maps[i];

		ret = omap_rproc_map_unmap(domain, me, true);
		if (ret) {
			dev_err(&pdev->dev, "iommu_map failed: %d\n", ret);
			goto unmap_regions;
		}
	}

	/* power on the remote processor itself */
	ret = omap_device_enable(pdev);
	if (ret)
		goto unmap_regions;

	return 0;

unmap_regions:
	for (--i; i >= 0 && rproc->memory_maps[i].size; i--) {
		const struct rproc_mem_entry *me = &rproc->memory_maps[i];
		omap_rproc_map_unmap(domain, me, false);
	}
free_domain:
	iommu_domain_free(domain);
free_mem:
	kfree(priv);
	return ret;
}

static inline int omap_rproc_stop(struct rproc *rproc)
{
	struct device *dev = rproc->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_rproc_priv *priv = rproc->priv;
	struct iommu_domain *domain = priv->domain;
	struct device *iommu = priv->iommu;
	int ret, i;

	/* power off the remote processor itself */
	ret = omap_device_shutdown(pdev);
	if (ret) {
		dev_err(dev, "failed to shutdown: %d\n", ret);
		goto out;
	}

	for (i = 0; rproc->memory_maps[i].size; i++) {
		const struct rproc_mem_entry *me = &rproc->memory_maps[i];

		ret = omap_rproc_map_unmap(domain, me, false);
		if (ret) {
			dev_err(&pdev->dev, "iommu_unmap failed: %d\n", ret);
			goto out;
		}
	}

	iommu_detach_device(domain, iommu);
	iommu_domain_free(domain);

out:
	return ret;
}

static struct rproc_ops omap_rproc_ops = {
	.start = omap_rproc_start,
	.stop = omap_rproc_stop,
};

static int omap_rproc_probe(struct platform_device *pdev)
{
	struct omap_rproc_pdata *pdata = pdev->dev.platform_data;

	return rproc_register(&pdev->dev, pdata->name, &omap_rproc_ops,
				pdata->firmware, pdata->memory_maps,
				THIS_MODULE);
}

static int __devexit omap_rproc_remove(struct platform_device *pdev)
{
	struct omap_rproc_pdata *pdata = pdev->dev.platform_data;

	return rproc_unregister(pdata->name);
}

static struct platform_driver omap_rproc_driver = {
	.probe = omap_rproc_probe,
	.remove = __devexit_p(omap_rproc_remove),
	.driver = {
		.name = "omap-rproc",
		.owner = THIS_MODULE,
	},
};

static int __init omap_rproc_init(void)
{
	return platform_driver_register(&omap_rproc_driver);
}
/* must be ready in time for device_initcall users */
subsys_initcall(omap_rproc_init);

static void __exit omap_rproc_exit(void)
{
	platform_driver_unregister(&omap_rproc_driver);
}
module_exit(omap_rproc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("OMAP Remote Processor control driver");
