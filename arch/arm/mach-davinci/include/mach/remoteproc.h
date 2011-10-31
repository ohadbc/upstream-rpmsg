/*
 * Remote Processor
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

#ifndef _DAVINCI_REMOTEPROC_H
#define _DAVINCI_REMOTEPROC_H

#include <linux/amp/remoteproc.h>

struct davinci_rproc_pdata {
	struct rproc_ops *ops;
	char *name;
	char *clk_name;
	char *firmware;
};

#endif /* _DAVINCI_REMOTEPROC_H */
