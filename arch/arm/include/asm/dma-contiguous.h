#ifndef ASMARM_DMA_CONTIGUOUS_H
#define ASMARM_DMA_CONTIGUOUS_H

#ifdef __KERNEL__

#include <linux/device.h>
#include <linux/dma-contiguous.h>

#ifdef CONFIG_CMA

#define MAX_CMA_AREAS	(8)

void dma_contiguous_early_fixup(phys_addr_t base, unsigned long size);

static inline struct cma *get_dev_cma_area(struct device *dev)
{
	if (dev && dev->archdata.cma_area)
		return dev->archdata.cma_area;
	return dma_contiguous_default_area;
}

static inline void set_dev_cma_area(struct device *dev, struct cma *cma)
{
	dev->archdata.cma_area = cma;
}

#else

#define MAX_CMA_AREAS	(0)

#endif
#endif
#endif
