/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2018 ST Microelectronics S.A.
 * Copyright 2019 Google Inc.
 */
#ifndef _UAPI_ST54J_SE_H
#define _UAPI_ST54J_SE_H

#define ST54J_SE_MAGIC	0xE5
/* ST54J_SE control via ioctl */
#define ST54J_SE_RESET            _IOR(ST54J_SE_MAGIC, 0x01, unsigned int)

#endif
