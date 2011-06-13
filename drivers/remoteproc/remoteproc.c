/*
 * Remote Processor Framework
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Ohad Ben-Cohen <ohad@wizery.com>
 * Mark Grosen <mgrosen@ti.com>
 * Brian Swetland <swetland@google.com>
 * Fernando Guzman Lugo <fernando.lugo@ti.com>
 * Robert Tivy <rtivy@ti.com>
 * Armando Uribe De Leon <x0095078@ti.com>
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
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/debugfs.h>
#include <linux/remoteproc.h>

/* list of the available remote processors */
static LIST_HEAD(rprocs);
/*
 * This lock should be taken when the list of rprocs is accessed.
 * Consider using RCU instead, since remote processors only get registered
 * once (usually at boot), and then the list is only read accessed.
 * Though right now the list is pretty short, and only rarely accessed.
 */
static DEFINE_SPINLOCK(rprocs_lock);

/* debugfs parent dir */
static struct dentry *rproc_dbg;

/*
 * Some remote processors may support dumping trace logs into a shared
 * memory buffer. We expose this trace buffer using debugfs, so users
 * can easily tell what's going on remotely.
 */
static ssize_t rproc_format_trace_buf(char __user *userbuf, size_t count,
				    loff_t *ppos, const void *src, int size)
{
	const char *buf = (const char *) src;
	int i;

	/*
	 * find the end of trace buffer (does not account for wrapping).
	 * desirable improvement: use a ring buffer instead.
	 */
	for (i = 0; i < size && buf[i]; i++);

	return simple_read_from_buffer(userbuf, count, ppos, src, i);
}

static int rproc_open_generic(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

#define DEBUGFS_READONLY_FILE(name, value, len)				\
static ssize_t name## _rproc_read(struct file *filp,			\
		char __user *userbuf, size_t count, loff_t *ppos)	\
{									\
	struct rproc *rproc = filp->private_data;			\
	return rproc_format_trace_buf(userbuf, count, ppos, value, len);\
}									\
									\
static const struct file_operations name ##_rproc_ops = {		\
	.read = name ##_rproc_read,					\
	.open = rproc_open_generic,					\
	.llseek	= generic_file_llseek,					\
};

/*
 * Currently we allow two trace buffers for each remote processor.
 * This is helpful in case a single remote processor has two independent
 * cores, each of which is running an independent OS image.
 * The current implementation is straightforward and simple, and is
 * rather limited to 2 trace buffers. If, in time, we'd realize we
 * need additional trace buffers, then the code should be refactored
 * and generalized.
 */
DEBUGFS_READONLY_FILE(trace0, rproc->trace_buf0, rproc->trace_len0);
DEBUGFS_READONLY_FILE(trace1, rproc->trace_buf1, rproc->trace_len1);

/* The state of the remote processor is exposed via debugfs, too */
const char *rproc_state_string(int state)
{
	const char *result;

	switch (state) {
	case RPROC_OFFLINE:
		result = "offline";
		break;
	case RPROC_SUSPENDED:
		result = "suspended";
		break;
	case RPROC_RUNNING:
		result = "running";
		break;
	case RPROC_LOADING:
		result = "loading";
		break;
	case RPROC_CRASHED:
		result = "crashed";
		break;
	default:
		result = "invalid state";
		break;
	}

	return result;
}

static ssize_t rproc_state_read(struct file *filp, char __user *userbuf,
						size_t count, loff_t *ppos)
{
	struct rproc *rproc = filp->private_data;
	int state = rproc->state;
	char buf[100];
	int i;

	i = snprintf(buf, 100, "%s (%d)\n", rproc_state_string(state), state);

	return simple_read_from_buffer(userbuf, count, ppos, buf, i);
}

static const struct file_operations rproc_state_ops = {
	.read = rproc_state_read,
	.open = rproc_open_generic,
	.llseek	= generic_file_llseek,
};

/* The name of the remote processor is exposed via debugfs, too */
static ssize_t rproc_name_read(struct file *filp, char __user *userbuf,
						size_t count, loff_t *ppos)
{
	struct rproc *rproc = filp->private_data;
	/* need room for the name, a newline and a terminating null */
	char buf[RPROC_MAX_NAME + 2];
	int i;

	i = snprintf(buf, RPROC_MAX_NAME + 2, "%s\n", rproc->name);

	return simple_read_from_buffer(userbuf, count, ppos, buf, i);
}

static const struct file_operations rproc_name_ops = {
	.read = rproc_name_read,
	.open = rproc_open_generic,
	.llseek	= generic_file_llseek,
};

#define DEBUGFS_ADD(name)						\
	debugfs_create_file(#name, 0400, rproc->dbg_dir,		\
			rproc, &name## _rproc_ops)

/**
 * __find_rproc_by_name() - find a registered remote processor by name
 * @name: name of the remote processor
 *
 * Internal function that returns the rproc @name, or NULL if @name does
 * not exists.
 */
static struct rproc *__find_rproc_by_name(const char *name)
{
	struct rproc *rproc;
	struct list_head *tmp;

	spin_lock(&rprocs_lock);

	list_for_each(tmp, &rprocs) {
		rproc = list_entry(tmp, struct rproc, next);
		if (!strcmp(rproc->name, name))
			break;
		rproc = NULL;
	}

	spin_unlock(&rprocs_lock);

	return rproc;
}

/**
 * __rproc_da_to_pa() - device to physical address conversion
 * @maps: the remote processor's memory mappings table
 * @da: a device address (as seen by the remote processor)
 * @pa: pointer where the physical address result should be stored
 *
 * This function converts @da to its physical address (pa) by going through
 * @maps, looking for a mapping that contains @da, and then calculating the
 * appropriate pa.
 *
 * Not all remote processors are behind an IOMMU, so if @maps is NULL,
 * we just return @da (after doing a basic sanity check).
 *
 * *** This function will be removed. Instead, iommu_iova_to_phys should
 * *** be used. This will be done once the OMAP IOMMU migration will
 * *** complete, and the missing parts in the generic IOMMU API will be
 * *** added.
 *
 * On success 0 is returned, and @pa is updated with the result.
 * Otherwise, -EINVAL is returned.
 */
static int
rproc_da_to_pa(const struct rproc_mem_entry *maps, u64 da, phys_addr_t *pa)
{
	int i;
	u64 offset;

	/*
	 * If we're not being an IOMMU, then the remoteproc is accessing
	 * physical addresses directly
	 */
	if (!maps) {
		if (da > ((phys_addr_t)(~0U)))
			return -EINVAL;

		*pa = (phys_addr_t) da;
		return 0;
	}

	for (i = 0; maps[i].size; i++) {
		const struct rproc_mem_entry *me = &maps[i];

		if (da >= me->da && da < (me->da + me->size)) {
			offset = da - me->da;
			pr_debug("%s: matched mem entry no. %d\n", __func__, i);
			*pa = me->pa + offset;
			return 0;
		}
	}

	return -EINVAL;
}

/**
 * rproc_start() - boot the remote processor
 * @rproc: the remote processor
 * @bootaddr: address of first instruction to execute (optional)
 *
 * Boot a remote processor (i.e. power it on, take it out of reset, etc..)
 */
static void rproc_start(struct rproc *rproc, u64 bootaddr)
{
	struct device *dev = rproc->dev;
	int err;

	err = mutex_lock_interruptible(&rproc->lock);
	if (err) {
		dev_err(dev, "can't lock remote processor %d\n", err);
		return;
	}

	err = rproc->ops->start(rproc, bootaddr);
	if (err) {
		dev_err(dev, "can't start rproc %s: %d\n", rproc->name, err);
		goto unlock_mutex;
	}

	rproc->state = RPROC_RUNNING;

	dev_info(dev, "remote processor %s is now up\n", rproc->name);

unlock_mutex:
	mutex_unlock(&rproc->lock);
}

/**
 * rproc_handle_trace_rsc() - handle a shared trace buffer resource
 * @rproc: the remote processor
 * @rsc: the trace resource descriptor
 *
 * In case the remote processor dumps trace logs into memory, ioremap it
 * and make it available to the user via debugfs.
 * Consider using the DMA mapping API here.
 *
 * Returns 0 on success, or an appropriate error code otherwise
 */
static int rproc_handle_trace_rsc(struct rproc *rproc, struct fw_resource *rsc)
{
	struct device *dev = rproc->dev;
	void *ptr;
	phys_addr_t pa;
	int ret;

	ret = rproc_da_to_pa(rproc->memory_maps, rsc->da, &pa);
	if (ret) {
		dev_err(dev, "invalid device address\n");
		return -EINVAL;
	}

	/* allow two trace buffers per rproc (can be extended if needed) */
	if (rproc->trace_buf0 && rproc->trace_buf1) {
		dev_warn(dev, "skipping extra trace rsc %s\n", rsc->name);
		return -EBUSY;
	}

	/*
	 * trace buffer memory is normal memory, so we cast away the __iomem
	 * to make sparse happy
	 */
	ptr = (__force void *) ioremap_nocache(pa, rsc->len);
	if (!ptr) {
		dev_err(dev, "can't ioremap trace buffer %s\n", rsc->name);
		return -ENOMEM;
	}

	/* add the trace0 debugfs entry. If it already exists, add trace1 */
	if (!rproc->trace_buf0) {
		rproc->trace_len0 = rsc->len;
		rproc->trace_buf0 = ptr;
		DEBUGFS_ADD(trace0);
	} else {
		rproc->trace_len1 = rsc->len;
		rproc->trace_buf1 = ptr;
		DEBUGFS_ADD(trace1);
	}

	return 0;
}

/**
 * rproc_handle_resources - go over and handle the resource section
 * @rproc: rproc handle
 * @rsc: the resource secion
 * @len: length of the resource section
 * @bootaddr: if found a boot address, put it here
 */
static int rproc_handle_resources(struct rproc *rproc, struct fw_resource *rsc,
							int len, u64 *bootaddr)
{
	struct device *dev = rproc->dev;
	int ret = 0;

	while (len >= sizeof(*rsc)) {
		dev_dbg(dev, "resource: type %d, da 0x%llx, pa 0x%llx, len 0x%x"
			", flags 0x%x, name %s\n", rsc->type, rsc->da, rsc->pa,
			rsc->len, rsc->flags, rsc->name);

		switch (rsc->type) {
		case RSC_TRACE:
			ret = rproc_handle_trace_rsc(rproc, rsc);
			if (ret)
				dev_err(dev, "failed handling rsc\n");
			break;
		case RSC_BOOTADDR:
			if (*bootaddr)
				dev_warn(dev, "bootaddr already set\n");
			*bootaddr = rsc->da;
			break;
		default:
			/* we don't support much yet, so don't be noisy */
			dev_dbg(dev, "unsupported resource %d\n", rsc->type);
			break;
		}

		if (ret)
			break;
		rsc++;
		len -= sizeof(*rsc);
	}

	/* unmap trace buffers on failure */
	if (ret && rproc->trace_buf0)
		iounmap((__force void __iomem *) rproc->trace_buf0);
	if (ret && rproc->trace_buf1)
		iounmap((__force void __iomem *) rproc->trace_buf1);

	return ret;
}

static int rproc_process_fw(struct rproc *rproc, struct fw_section *section,
						int left, u64 *bootaddr)
{
	struct device *dev = rproc->dev;
	phys_addr_t pa;
	u32 len, type;
	u64 da;
	int ret = 0;
	void *ptr;

	while (left > sizeof(struct fw_section)) {
		da = section->da;
		len = section->len;
		type = section->type;

		dev_dbg(dev, "section: type %d da 0x%llx len 0x%x\n",
								type, da, len);

		left -= sizeof(struct fw_section);
		if (left < section->len) {
			dev_err(dev, "firmware image is truncated\n");
			ret = -EINVAL;
			break;
		}

		ret = rproc_da_to_pa(rproc->memory_maps, da, &pa);
		if (ret) {
			dev_err(dev, "rproc_da_to_pa failed: %d\n", ret);
			break;
		}

		dev_dbg(dev, "da 0x%llx pa 0x%x len 0x%x\n", da, pa, len);

		/* ioremaping normal memory, so make sparse happy */
		ptr = (__force void *) ioremap_nocache(pa, len);
		if (!ptr) {
			dev_err(dev, "can't ioremap 0x%x\n", pa);
			ret = -ENOMEM;
			break;
		}

		/* put the section where the remoteproc will expect it */
		memcpy(ptr, section->content, len);

		/* a resource table needs special handling */
		if (section->type == FW_RESOURCE)
			ret = rproc_handle_resources(rproc,
						(struct fw_resource *) ptr,
						len, bootaddr);

		/* iounmap normal memory; make sparse happy */
		iounmap((__force void __iomem *) ptr);

		/* rproc_handle_resources may have failed */
		if (ret)
			break;

		section = (struct fw_section *)(section->content + len);
		left -= len;
	}

	return ret;
}

static void rproc_load_fw(const struct firmware *fw, void *context)
{
	struct rproc *rproc = context;
	struct device *dev = rproc->dev;
	const char *fwfile = rproc->firmware;
	u64 bootaddr = 0;
	struct fw_header *image;
	struct fw_section *section;
	int left, ret;

	if (!fw) {
		dev_err(dev, "%s: failed to load %s\n", __func__, fwfile);
		goto complete_fw;
	}

	dev_info(dev, "Loaded fw image %s, size %d\n", fwfile, fw->size);

	/* make sure this image is sane */
	if (fw->size < sizeof(struct fw_header)) {
		dev_err(dev, "Image is too small\n");
		goto out;
	}

	image = (struct fw_header *) fw->data;

	if (memcmp(image->magic, "RPRC", 4)) {
		dev_err(dev, "Image is corrupted (bad magic)\n");
		goto out;
	}

	dev_info(dev, "BIOS image version is %d\n", image->version);

	/* now process the image, section by section */
	section = (struct fw_section *)(image->header + image->header_len);

	left = fw->size - sizeof(struct fw_header) - image->header_len;

	ret = rproc_process_fw(rproc, section, left, &bootaddr);
	if (ret) {
		dev_err(dev, "Failed to process the image: %d\n", ret);
		goto out;
	}

	rproc_start(rproc, bootaddr);

out:
	release_firmware(fw);
complete_fw:
	/* allow all contexts calling rproc_put() to proceed */
	complete_all(&rproc->firmware_loading_complete);
}

/**
 * rproc_get() - boot the remote processor
 * @name: name of the remote processor
 *
 * Boot a remote processor (i.e. load its firmware, power it on, take it
 * out of reset, etc..).
 *
 * If the remote processor is already powered on, immediately return
 * its rproc handle.
 *
 * On success, returns the rproc handle. On failure, NULL is returned.
 */
struct rproc *rproc_get(const char *name)
{
	struct rproc *rproc, *ret = NULL;
	struct device *dev;
	int err;

	rproc = __find_rproc_by_name(name);
	if (!rproc) {
		pr_err("can't find remote processor %s\n", name);
		return NULL;
	}

	dev = rproc->dev;

	err = mutex_lock_interruptible(&rproc->lock);
	if (err) {
		dev_err(dev, "can't lock remote processor %s\n", name);
		return NULL;
	}

	/* prevent underlying implementation from being removed */
	if (!try_module_get(rproc->owner)) {
		dev_err(dev, "%s: can't get owner\n", __func__);
		goto unlock_mutex;
	}

	/* skip the boot process if rproc is already (being) powered up */
	if (rproc->count++) {
		ret = rproc;
		goto unlock_mutex;
	}

	/* rproc_put() calls should wait until async loader completes */
	init_completion(&rproc->firmware_loading_complete);

	dev_info(dev, "powering up %s\n", name);

	/* loading a firmware is required */
	if (!rproc->firmware) {
		dev_err(dev, "%s: no firmware to load\n", __func__);
		goto deref_rproc;
	}

	/*
	 * Initiate an asynchronous firmware loading, to allow building
	 * remoteproc as built-in kernel code, without hanging the boot process
	 */
	err = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG,
			rproc->firmware, dev, GFP_KERNEL, rproc, rproc_load_fw);
	if (err < 0) {
		dev_err(dev, "request_firmware_nowait failed: %d\n", err);
		goto deref_rproc;
	}

	rproc->state = RPROC_LOADING;
	ret = rproc;
	goto unlock_mutex;

deref_rproc:
	complete_all(&rproc->firmware_loading_complete);
	module_put(rproc->owner);
	--rproc->count;
unlock_mutex:
	mutex_unlock(&rproc->lock);
	return ret;
}
EXPORT_SYMBOL(rproc_get);

/**
 * rproc_put() - power off the remote processor
 * @rproc: the remote processor
 *
 * Release an rproc handle previously acquired with rproc_get(),
 * and if we're the last user, power the processor off.
 *
 * Every call to rproc_get() must be (eventually) accompanied by a call
 * to rproc_put(). Calling rproc_put() redundantly is a bug.
 */
void rproc_put(struct rproc *rproc)
{
	struct device *dev = rproc->dev;
	int ret;

	/*
	 * make sure rproc_get() was called beforehand.
	 * it should be safe to check for zero without taking the lock.
	 */
	if (!rproc->count) {
		dev_err(dev, "asymmetric put (fogot to call rproc_get ?)\n");
		ret = -EINVAL;
		goto out;
	}

	/* if rproc is just being loaded now, wait */
	wait_for_completion(&rproc->firmware_loading_complete);

	ret = mutex_lock_interruptible(&rproc->lock);
	if (ret) {
		dev_err(dev, "can't lock rproc %s: %d\n", rproc->name, ret);
		return;
	}

	/* if the remote proc is still needed, bail out */
	if (--rproc->count)
		goto out;

	if (rproc->trace_buf0)
		/* iounmap normal memory, so make sparse happy */
		iounmap((__force void __iomem *) rproc->trace_buf0);
	if (rproc->trace_buf1)
		/* iounmap normal memory, so make sparse happy */
		iounmap((__force void __iomem *) rproc->trace_buf1);

	rproc->trace_buf0 = rproc->trace_buf1 = NULL;

	/*
	 * make sure rproc is really running before powering it off.
	 * this is important, because the fw loading might have failed.
	 */
	if (rproc->state == RPROC_RUNNING) {
		ret = rproc->ops->stop(rproc);
		if (ret) {
			dev_err(dev, "can't stop rproc: %d\n", ret);
			goto out;
		}
	}

	rproc->state = RPROC_OFFLINE;

	dev_info(dev, "stopped remote processor %s\n", rproc->name);

out:
	mutex_unlock(&rproc->lock);
	if (!ret)
		module_put(rproc->owner);
}
EXPORT_SYMBOL(rproc_put);

/**
 * rproc_register() - register a remote processor
 * @dev: the underlying device
 * @name: name of this remote processor
 * @ops: platform-specific handlers (mainly start/stop)
 * @firmware: name of firmware file to load
 * @memory_maps: IOMMU settings for this rproc (optional)
 * @owner: owning module
 *
 * Registers a new remote processor in the remoteproc framework.
 *
 * This is called by the underlying platform-specific implementation,
 * whenever a new remote processor device is probed.
 *
 * On succes, 0 is return, and on failure an appropriate error code.
 */
int rproc_register(struct device *dev, const char *name,
				const struct rproc_ops *ops,
				const char *firmware,
				const struct rproc_mem_entry *memory_maps,
				struct module *owner)
{
	struct rproc *rproc;

	if (!dev || !name || !ops)
		return -EINVAL;

	rproc = kzalloc(sizeof(struct rproc), GFP_KERNEL);
	if (!rproc) {
		dev_err(dev, "%s: kzalloc failed\n", __func__);
		return -ENOMEM;
	}

	rproc->dev = dev;
	rproc->name = name;
	rproc->ops = ops;
	rproc->firmware = firmware;
	rproc->memory_maps = memory_maps;
	rproc->owner = owner;

	mutex_init(&rproc->lock);

	rproc->state = RPROC_OFFLINE;

	spin_lock(&rprocs_lock);
	list_add_tail(&rproc->next, &rprocs);
	spin_unlock(&rprocs_lock);

	dev_info(dev, "%s is available\n", name);

	if (!rproc_dbg)
		goto out;

	rproc->dbg_dir = debugfs_create_dir(dev_name(dev), rproc_dbg);
	if (!rproc->dbg_dir) {
		dev_err(dev, "can't create debugfs dir\n");
		goto out;
	}

	debugfs_create_file("name", 0400, rproc->dbg_dir, rproc,
							&rproc_name_ops);
	debugfs_create_file("state", 0400, rproc->dbg_dir, rproc,
							&rproc_state_ops);

out:
	return 0;
}
EXPORT_SYMBOL(rproc_register);

/**
 * rproc_unregister() - unregister a remote processor
 * @name: name of this remote processor
 *
 * Unregisters a remote processor.
 *
 * On succes, 0 is return. If this remote processor isn't found, -EINVAL
 * is returned.
 */
int rproc_unregister(const char *name)
{
	struct rproc *rproc;

	rproc = __find_rproc_by_name(name);
	if (!rproc) {
		pr_err("can't find remote processor %s\n", name);
		return -EINVAL;
	}

	dev_info(rproc->dev, "removing %s\n", name);

	if (rproc->dbg_dir)
		debugfs_remove_recursive(rproc->dbg_dir);

	spin_lock(&rprocs_lock);
	list_del(&rproc->next);
	spin_unlock(&rprocs_lock);

	kfree(rproc);

	return 0;
}
EXPORT_SYMBOL(rproc_unregister);

static int __init remoteproc_init(void)
{
	if (debugfs_initialized()) {
		rproc_dbg = debugfs_create_dir(KBUILD_MODNAME, NULL);
		if (!rproc_dbg)
			pr_err("can't create debugfs dir\n");
	}

	return 0;
}
/* must be ready in time for device_initcall users */
subsys_initcall(remoteproc_init);

static void __exit remoteproc_exit(void)
{
	if (rproc_dbg)
		debugfs_remove(rproc_dbg);
}
module_exit(remoteproc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Generic Remote Processor Framework");
