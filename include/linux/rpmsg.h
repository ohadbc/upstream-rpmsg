/*
 * Remote processor messaging
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
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

#ifndef _LINUX_RPMSG_H
#define _LINUX_RPMSG_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>

/* The feature bitmap for virtio rpmsg */
#define VIRTIO_RPMSG_F_NS	0 /* RP supports name service notifications */

/**
 * struct rpmsg_hdr - common header for all rpmsg messages
 * @src: source address
 * @dst: destination address
 * @reserved: reserved for future use
 * @len: length of payload (in bytes)
 * @flags: message flags
 * @data: @len bytes of message payload data
 *
 * Every message sent(/received) on the rpmsg bus begins with this header.
 */
struct rpmsg_hdr {
	u32 src;
	u32 dst;
	u32 reserved;
	u16 len;
	u16 flags;
	u8 data[0];
} __packed;

/**
 * struct rpmsg_ns_msg - dynamic name service announcement message
 * @name: name of remote service that is published
 * @addr: address of remote service that is published
 * @flags: indicates whether service is created or destroyed
 *
 * This message is sent across to publish a new service (or announce
 * about its removal). When Linux receives these messages, an appropriate
 * rpmsg channel (i.e device) is created/destroyed. In turn, the ->probe()
 * or ->remove() handler of the appropriate rpmsg driver will be invoked
 * (if/as-soon-as one is registered).
 */
struct rpmsg_ns_msg {
	char name[RPMSG_NAME_SIZE];
	u32 addr;
	u32 flags;
} __packed;

/**
 * struct rpmsg_ns_flags - dynamic name service announcement flags
 *
 * @RPMSG_NS_CREATE: a new remote service was just created
 * @RPMSG_NS_DESTROY: a known remote service was just destroyed
 */
enum rpmsg_ns_flags {
	RPMSG_NS_CREATE		= 0,
	RPMSG_NS_DESTROY	= 1,
};

/**
 * enum rpmsg_platform_requests - platform-specific rpmsg configuration requests
 *
 * These configuration requests are required by the rpmsg bus, and must be
 * implemented by the platform-specific rpmsg backend.
 *
 * @VPROC_BUF_ADDR: Kernel virtual address of an uncached memory region,
 *		    shared with the remote processor, that will be splitted
 *		    to buffers and then used to send messages across to the
 *		    remote processor. Those buffers will be added to the
 *		    appropriate vrings by the rpmsg bus in order to send and
 *		    receive messages.
 *
 * @VPROC_BUF_PADDR: Physical address of the above memory region, needed
 *		     for the "wire" protocol between the two processors
 *		     (i.e. for the vring's buffer descriptors).
 *
 * @VPROC_BUF_NUM: Number of buffers to split the shared memory region to
 * @VPROC_BUF_SZ: Size of each buffer that the shared memory region will be
 *		  split into. It is the responsibility of the underlying
 *		  implementation to make sure that the size of the memory
 *		  region provided by @VPROC_BUF_ADDR is exactly
 *		  @VPROC_BUF_NUM * @VPROC_BUF_SZ bytes.
 *
 * @VPROC_STATIC_CHANNELS: Table of static channels that this platform
 *			   expects to have. See struct rpmsg_channel_info
 *			   for additional information.
 *			   This configuration is optional: it is perfectly
 *			   fine not to have any pre-configured static channels.
 *
 * The number and size of buffers to use are considered platform-specific,
 * because this is strongly tied with the performance/functionality
 * requirements of the specific use cases that the platform needs rpmsg
 * for. This should be revisited when we'll have a bigger requirement
 * picture.
 *
 * We might also want to add support for user-provided buffers. This will
 * allow bigger buffer size flexibility, and might also be used to achieve
 * zero-copy messaging.
 */
enum rpmsg_platform_requests {
	VPROC_BUF_ADDR,
	VPROC_BUF_PADDR,
	VPROC_BUF_NUM,
	VPROC_BUF_SZ,
	VPROC_STATIC_CHANNELS,
};

#define RPMSG_ADDR_ANY		0xFFFFFFFF

struct virtproc_info;

/**
 * rpmsg_channel - the rpmsg bus devices are called channels
 * @vrp: the remote processor this channel belongs to
 * @dev: the device struct
 * @id: device id (used to match between rpmsg drivers and devices)
 * @src: local address
 * @dst: destination address
 * @ept: the rpmsg endpoint of this channel
 * @announce: if set, rpmsg will announce the creation/removal of this channel
 */
struct rpmsg_channel {
	struct virtproc_info *vrp;
	struct device dev;
	struct rpmsg_device_id id;
	u32 src;
	u32 dst;
	struct rpmsg_endpoint *ept;
	bool announce;
};

/**
 * struct rpmsg_channel_info - static channel info
 * @name: name of service
 * @src: local address
 * @dst: destination address
 *
 * This struct is used to define static channel information, namely name
 * and addresses, which is used by platform-specific code to create static
 * channels (i.e. channels that always exists).
 *
 * Use cases of static channels are:
 *
 * 1. In case the remote processor does not support dynamic name service
 *    announcements (i.e. remote channel creation/removal).
 *    In this case, we predefine a static channel table which contains the
 *    remote rpmsg services supported by the remote processor. The rpmsg
 *    bus then iterates through this table, and creates rpmsg channels
 *    (i.e. devices) accordingly. @dst will contain the rpmsg address
 *    of the remote service, and @src will most likely contain RPMSG_ADDR_ANY.
 *
 * 2. To define channels which will be probed with local rpmsg "server"
 *    driver (i.e. drivers that expose a service). In this case, @src will
 *    contain the local rpmsg address of the service (but this can made
 *    dynamic too, if remote processor supports listening to our own dynamic
 *    name service announcements), and @dst will most likely be RPMSG_ADDR_ANY.
 *
 * Use the RPMSG_REMOTE_CHNL and RMSG_SERVER_CHNL macros (see below)
 * to populate the static channels table for the above two use cases.
 */
struct rpmsg_channel_info {
	char name[RPMSG_NAME_SIZE];
	u32 src;
	u32 dst;
};

/*
 * rpmsg server channels have a local predefined address, and accept
 * any remote address
 */
#define RMSG_SERVER_CHNL(name, addr)	name, addr, RPMSG_ADDR_ANY

/*
 * rpmsg remote service channels have a remote predefined address, and
 * don't care about the local address
 */
#define RMSG_REMOTE_CHNL(name, addr)	name, RPMSG_ADDR_ANY, addr

/**
 * struct rpmsg_endpoint - binds a local rpmsg address to its user
 * @rpdev: rpmsg channel device
 * @cb: rx callback handler
 * @addr: local rpmsg address
 * @priv: private data for the driver's use
 *
 * In essence, an rpmsg endpoint represents a listener on the rpmsg bus, as
 * it binds together an rpmsg address with an rx callback handler.
 *
 * Simple rpmsg drivers shouldn't be aware of this detail, because
 * things just work: every rpmsg driver provides an rx callback upon
 * registering to the bus, and that callback is then bound to its rpmsg
 * address when the driver is probed. When relevant inbound messages arrive
 * (i.e. messages which their dst address equals to the src address of
 * the rpmsg channel), the driver's handler is invoked to process it.
 *
 * More complicated drivers though, that do need to allocate additional rpmsg
 * addresses, and bind them to different rx callbacks, must explicitly
 * create themselves additional endpoints (see rpmsg_create_ept()).
 */
struct rpmsg_endpoint {
	struct rpmsg_channel *rpdev;
	void (*cb)(struct rpmsg_channel *, void *, int, void *, u32);
	u32 addr;
	void *priv;
};

/**
 * struct rpmsg_driver - rpmsg driver struct
 * @drv: underlying device driver
 * @id_table: rpmsg ids serviced by this driver
 * @probe: invoked when a matching rpmsg channel (i.e. device) is found
 * @remove: invoked when the rpmsg channel is removed
 * @callback: invoked when an inbound message is received on the channel
 */
struct rpmsg_driver {
	struct device_driver drv;
	const struct rpmsg_device_id *id_table;
	int (*probe)(struct rpmsg_channel *dev);
	void (*remove)(struct rpmsg_channel *dev);
	void (*callback)(struct rpmsg_channel *, void *, int, void *, u32);
};

int register_rpmsg_device(struct rpmsg_channel *dev);
void unregister_rpmsg_device(struct rpmsg_channel *dev);
int register_rpmsg_driver(struct rpmsg_driver *drv);
void unregister_rpmsg_driver(struct rpmsg_driver *drv);
void rpmsg_destroy_ept(struct rpmsg_endpoint *);
struct rpmsg_endpoint *rpmsg_create_ept(struct rpmsg_channel *,
		void (*cb)(struct rpmsg_channel *, void *, int, void *, u32),
		void *priv, u32 addr);

int
rpmsg_send_offchannel_raw(struct rpmsg_channel *, u32, u32, void *, int, bool);

/**
 * rpmsg_send() - send a message across to the remote processor
 * @rpdev: the rpmsg channel
 * @data: payload of message
 * @len: length of payload
 *
 * This function sends @data of length @len on the @rpdev channel.
 * The message will be sent to the remote processor which the @rpdev
 * channel belongs to, using @rpdev's source and destination addresses.
 * In case there are no TX buffers available, the function will block until
 * one becomes available, or a timeout of 15 seconds elapses. When the latter
 * happens, -ERESTARTSYS is returned.
 *
 * Can only be called from process context (for now).
 *
 * Returns 0 on success and an appropriate error value on failure.
 */
static inline int rpmsg_send(struct rpmsg_channel *rpdev, void *data, int len)
{
	u32 src = rpdev->src, dst = rpdev->dst;

	return rpmsg_send_offchannel_raw(rpdev, src, dst, data, len, true);
}

/**
 * rpmsg_sendto() - send a message across to the remote processor, specify dst
 * @rpdev: the rpmsg channel
 * @data: payload of message
 * @len: length of payload
 * @dst: destination address
 *
 * This function sends @data of length @len to the remote @dst address.
 * The message will be sent to the remote processor which the @rpdev
 * channel belongs to, using @rpdev's source address.
 * In case there are no TX buffers available, the function will block until
 * one becomes available, or a timeout of 15 seconds elapses. When the latter
 * happens, -ERESTARTSYS is returned.
 *
 * Can only be called from process context (for now).
 *
 * Returns 0 on success and an appropriate error value on failure.
 */
static inline
int rpmsg_sendto(struct rpmsg_channel *rpdev, void *data, int len, u32 dst)
{
	u32 src = rpdev->src;

	return rpmsg_send_offchannel_raw(rpdev, src, dst, data, len, true);
}

/**
 * rpmsg_send_offchannel() - send a message using explicit src/dst addresses
 * @rpdev: the rpmsg channel
 * @src: source address
 * @dst: destination address
 * @data: payload of message
 * @len: length of payload
 *
 * This function sends @data of length @len to the remote @dst address,
 * and uses @src as the source address.
 * The message will be sent to the remote processor which the @rpdev
 * channel belongs to.
 * In case there are no TX buffers available, the function will block until
 * one becomes available, or a timeout of 15 seconds elapses. When the latter
 * happens, -ERESTARTSYS is returned.
 *
 * Can only be called from process context (for now).
 *
 * Returns 0 on success and an appropriate error value on failure.
 */
static inline
int rpmsg_send_offchannel(struct rpmsg_channel *rpdev, u32 src, u32 dst,
							void *data, int len)
{
	return rpmsg_send_offchannel_raw(rpdev, src, dst, data, len, true);
}

/**
 * rpmsg_send() - send a message across to the remote processor
 * @rpdev: the rpmsg channel
 * @data: payload of message
 * @len: length of payload
 *
 * This function sends @data of length @len on the @rpdev channel.
 * The message will be sent to the remote processor which the @rpdev
 * channel belongs to, using @rpdev's source and destination addresses.
 * In case there are no TX buffers available, the function will immediately
 * return -ENOMEM without waiting until one becomes available.
 *
 * Can only be called from process context (for now).
 *
 * Returns 0 on success and an appropriate error value on failure.
 */
static inline
int rpmsg_trysend(struct rpmsg_channel *rpdev, void *data, int len)
{
	u32 src = rpdev->src, dst = rpdev->dst;

	return rpmsg_send_offchannel_raw(rpdev, src, dst, data, len, false);
}

/**
 * rpmsg_sendto() - send a message across to the remote processor, specify dst
 * @rpdev: the rpmsg channel
 * @data: payload of message
 * @len: length of payload
 * @dst: destination address
 *
 * This function sends @data of length @len to the remote @dst address.
 * The message will be sent to the remote processor which the @rpdev
 * channel belongs to, using @rpdev's source address.
 * In case there are no TX buffers available, the function will immediately
 * return -ENOMEM without waiting until one becomes available.
 *
 * Can only be called from process context (for now).
 *
 * Returns 0 on success and an appropriate error value on failure.
 */
static inline
int rpmsg_trysendto(struct rpmsg_channel *rpdev, void *data, int len, u32 dst)
{
	u32 src = rpdev->src;

	return rpmsg_send_offchannel_raw(rpdev, src, dst, data, len, false);
}

/**
 * rpmsg_send_offchannel() - send a message using explicit src/dst addresses
 * @rpdev: the rpmsg channel
 * @src: source address
 * @dst: destination address
 * @data: payload of message
 * @len: length of payload
 *
 * This function sends @data of length @len to the remote @dst address,
 * and uses @src as the source address.
 * The message will be sent to the remote processor which the @rpdev
 * channel belongs to.
 * In case there are no TX buffers available, the function will immediately
 * return -ENOMEM without waiting until one becomes available.
 *
 * Can only be called from process context (for now).
 *
 * Returns 0 on success and an appropriate error value on failure.
 */
static inline
int rpmsg_trysend_offchannel(struct rpmsg_channel *rpdev, u32 src, u32 dst,
							void *data, int len)
{
	return rpmsg_send_offchannel_raw(rpdev, src, dst, data, len, false);
}

#endif /* _LINUX_RPMSG_H */
