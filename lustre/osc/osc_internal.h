/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2003 Cluster File Systems, Inc.
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 */

#ifndef OSC_INTERNAL_H
#define OSC_INTERNAL_H

#include <portals/lib-types.h> /* for PTL_MTU and PTL_MD_MAX_PAGES */

/* bug 1578: negotiate BRW_MAX_SIZE with the OST, instead of hard-coding it */
#define OSC_BRW_MAX_SIZE PTL_MTU
#define OSC_BRW_MAX_IOV PTL_MD_MAX_PAGES

#define OAP_MAGIC 8675309

struct osc_async_page {
        int                     oap_magic;
        struct list_head        oap_pending_item;
        struct list_head        oap_urgent_item;
        struct list_head        oap_rpc_item;
        struct page             *oap_page;
        int                     oap_cmd;

        obd_off                 oap_obj_off;
        obd_off                 oap_page_off;
        int                     oap_count;
        obd_flag                oap_brw_flags;
        enum async_flags        oap_async_flags;

        struct obd_sync_io_container *oap_osic;

	struct obd_async_page_ops *oap_caller_ops;
        void                   *oap_caller_data;
};

#define OSCC_FLAG_RECOVERING 1
#define OSCC_FLAG_CREATING   2
#define OSCC_FLAG_NOSPC      4 /* can't create more objects on this OST */

int osc_create(struct obd_export *exp, struct obdo *oa,
	       struct lov_stripe_md **ea, struct obd_trans_info *oti);
int osc_real_create(struct obd_export *exp, struct obdo *oa,
	       struct lov_stripe_md **ea, struct obd_trans_info *oti);
void oscc_init(struct obd_export *exp);

extern atomic_t osc_max_rpcs_in_flight;
extern atomic_t osc_max_pages_per_rpc;

#ifdef __KERNEL__

int lproc_osc_attach_seqstat(struct obd_device *dev);
void lproc_osc_hist(struct osc_histogram *oh, unsigned int value);
void lproc_osc_hist_pow2(struct osc_histogram *oh, unsigned int value);

#else /* !__KERNEL__ */

#define lproc_osc_attach_seqstat(dev) (0)
#define lproc_osc_hist(o,v) do{}while(0)
#define lproc_osc_hist_pow2(o, v) do{}while(0)

#endif

#endif /* OSC_INTERNAL_H */
