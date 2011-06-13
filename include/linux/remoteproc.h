/*
 * Remote Processor Framework
 *
 * Copyright(c) 2011 Texas Instruments, Inc.
 * Copyright(c) 2011 Google, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name Texas Instruments nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef REMOTEPROC_H
#define REMOTEPROC_H

#include <linux/mutex.h>
#include <linux/completion.h>

/**
 * DOC: The Binary Structure of the Firmware
 *
 * The following enums and structures define the binary format of the image
 * we load and run the remote processors with.
 *
 * The binary format is as follows:
 *
 * struct {
 *     char magic[4] = { 'R', 'P', 'R', 'C' };
 *     u32 version;
 *     u32 header_len;
 *     char header[...] = { header_len bytes of unformatted, textual header };
 *     struct section {
 *         u32 type;
 *         u64 da;
 *         u32 len;
 *         u8 content[...] = { len bytes of binary data };
 *     } [ no limit on number of sections ];
 * } __packed;
 */

/**
 * struct fw_header - header of the firmware image
 * @magic: 4-bytes magic (should contain "RPRC")
 * @version: version number, should be bumped on binary changes
 * @header_len: length, in bytes, of the following text header
 * @header: free-style textual header, users can read with 'head'
 *
 * This structure defines the header of the remoteproc firmware.
 */
struct fw_header {
	char magic[4];
	u32 version;
	u32 header_len;
	char header[0];
} __packed;

/**
 * struct fw_section - header of a firmware section
 * @type: section type
 * @da: device address that the rproc expects to find this section at.
 * @len: length of the section (in bytes)
 * @content: the section data
 *
 * This structure defines the header of a firmware section. All sections
 * should be loaded to the address specified by @da, so the remote processor
 * will find them.
 *
 * Note: if the remote processor is not behind an IOMMU, then da is a
 * mere physical address
 */
struct fw_section {
	u32 type;
	u64 da;
	u32 len;
	char content[0];
} __packed;

/**
 * enum fw_section_type - section type values
 *
 * @FW_RESOURCE: a resource section. this section contains static
 *		resource requests (/announcements) that the remote
 *		processor requires (/supports). Most of these requests
 *		require that the host fulfill them (and usually
 *		"reply" with a result) before the remote processor
 *		is booted. See Documentation/remoteproc.h for more info
 * @FW_TEXT: a text section
 * @FW_DATA: a data section
 *
 * Note: text and data sections have different types so we can support stuff
 * like crash dumps (which only requires dumping data sections) or loading
 * text sections into faster memory. Currently, though, both section types
 * are treated exactly the same.
 */
enum fw_section_type {
	FW_RESOURCE	= 0,
	FW_TEXT		= 1,
	FW_DATA		= 2,
};

/**
 * struct fw_resource - describes an entry from the resource section
 * @type: resource type
 * @da: depends on the resource type
 * @pa: depends on the resource type
 * @len: depends on the resource type
 * @flags: depends on the resource type
 * @name: name of resource
 *
 * Some resources entries are mere announcements, where the host is informed
 * of specific remoteproc configuration. Other entries require the host to
 * do something (e.g. reserve a requested resource) and reply by overwriting
 * a member inside struct fw_resource with the id of the allocated resource.
 * There could also be resource entries where the remoteproc's image suggests
 * a configuration, but the host may overwrite it with its own preference.
 *
 * Note: the vast majority of the resource types are not implemented yet,
 * and this is all very much preliminary.
 */
struct fw_resource {
	u32 type;
	u64 da;
	u64 pa;
	u32 len;
	u32 flags;
	u8 name[48];
} __packed;

/**
 * enum fw_resource_type - types of resource entries
 *
 * @RSC_TRACE: announces the availability of a trace buffer into which
 *		the remote processor will be writing logs. In this case,
 *		'da' indicates the device address where logs are written to,
 *		and 'len' is the size of the trace buffer.
 *		Currently we support two trace buffers per remote processor,
 *		to support two autonomous cores running in a single rproc
 *		device.
 *		If additional trace buffers are needed, this should be
 *		extended/generalized.
 * @RSC_BOOTADDR: announces the address of the first instruction the remote
 *		processor should be booted with (address indicated in 'da').
 *
 * Note: most of the resource types are not implemented yet, so they are
 * not documented yet.
 */
enum fw_resource_type {
	RSC_CARVEOUT	= 0,
	RSC_DEVMEM	= 1,
	RSC_DEVICE	= 2,
	RSC_IRQ		= 3,
	RSC_TRACE	= 4,
	RSC_BOOTADDR	= 5,
};

/**
 * struct rproc_mem_entry - memory mapping descriptor
 * @da:		device address as seen by the remote processor
 * @pa:		physical address
 * @size:	size of this memory region
 *
 * Board file will use this struct to define the IOMMU configuration
 * for this remote processor. If the rproc device accesses physical memory
 * directly (and not through an IOMMU), this is not needed.
 */
struct rproc_mem_entry {
	u64 da;
	phys_addr_t pa;
	u32 size;
};

struct rproc;

/**
 * struct rproc_ops - platform-specific device handlers
 * @start:	power on the device and boot it. implementation may require
 *		specifyng a boot address
 * @stop:	power off the device
 */
struct rproc_ops {
	int (*start)(struct rproc *rproc, u64 bootaddr);
	int (*stop)(struct rproc *rproc);
};

/*
 * enum rproc_state - remote processor states
 *
 * @RPROC_OFFLINE:	device is powered off
 * @RPROC_SUSPENDED:	device is suspended; needs to be woken up to receive
 *			a message.
 * @RPROC_RUNNING:	device is up and running
 * @RPROC_LOADING:	asynchronous firmware loading has started
 * @RPROC_CRASHED: 	device has crashed; need to start recovery
 */
enum rproc_state {
	RPROC_OFFLINE,
	RPROC_SUSPENDED,
	RPROC_RUNNING,
	RPROC_LOADING,
	RPROC_CRASHED,
};

#define RPROC_MAX_NAME	100

/*
 * struct rproc - represents a physical remote processor device
 *
 * @next: next rproc entry in the list
 * @name: human readable name of the rproc, cannot exceed RPROC_MAX_NAME bytes
 * @memory_maps: table of da-to-pa memory maps (relevant if device is behind
 *               an iommu)
 * @firmware: name of firmware file to be loaded
 * @owner: reference to the platform-specific rproc module
 * @priv: private data which belongs to the platform-specific rproc module
 * @ops: platform-specific start/stop rproc handlers
 * @dev: underlying device
 * @count: usage refcount
 * @state: state of the device
 * @lock: lock which protects concurrent manipulations of the rproc
 * @dbg_dir: debugfs directory of this rproc device
 * @trace_buf0: main trace buffer of the remote processor
 * @trace_buf1: second, optional, trace buffer of the remote processor
 * @trace_len0: length of main trace buffer of the remote processor
 * @trace_len1: length of the second (and optional) trace buffer
 * @firmware_loading_complete: marks e/o asynchronous firmware loading
 */
struct rproc {
	struct list_head next;
	const char *name;
	const struct rproc_mem_entry *memory_maps;
	const char *firmware;
	struct module *owner;
	void *priv;
	const struct rproc_ops *ops;
	struct device *dev;
	int count;
	int state;
	struct mutex lock;
	struct dentry *dbg_dir;
	char *trace_buf0, *trace_buf1;
	int trace_len0, trace_len1;
	struct completion firmware_loading_complete;
};

struct rproc *rproc_get(const char *);
void rproc_put(struct rproc *);
int rproc_register(struct device *, const char *, const struct rproc_ops *,
		const char *, const struct rproc_mem_entry *, struct module *);
int rproc_unregister(const char *);

#endif /* REMOTEPROC_H */
