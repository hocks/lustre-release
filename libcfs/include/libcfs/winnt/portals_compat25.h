/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef __LIBCFS_WINNT_PORTALS_COMPAT_H__
#define __LIBCFS_WINNT_PORTALS_COMPAT_H__
#ifdef __KERNEL__
/*
 * Signal
 */

#define SIGNAL_MASK_LOCK(task, flags)           do {} while(0)
#define SIGNAL_MASK_UNLOCK(task, flags)         do {} while(0)
#define call_usermodehelper(path, argv, envp, 1)	do {} while(0)
#define recalc_sigpending()				do {} while(0)
#define clear_tsk_thread_flag(current, TIF_SIGPENDING)	do {} while(0)
#endif

#define LL_PROC_PROTO(name)						\
	name(struct ctl_table *table, int write, struct file *filp,	\
	     void __user *buffer, size_t *lenp)

#endif /* _PORTALS_COMPAT_H */
