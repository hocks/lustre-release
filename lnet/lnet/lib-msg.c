/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * lib/lib-msg.c
 * Message decoding, parsing and finalizing routines
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define DEBUG_SUBSYSTEM S_PORTALS

#include <lnet/lib-lnet.h>

void
lnet_enq_event_locked (lnet_eq_t *eq, lnet_event_t *ev)
{
        lnet_event_t  *eq_slot;

        /* Allocate the next queue slot */
        ev->link = ev->sequence = eq->eq_enq_seq++;
        /* NB we don't support START events yet and we don't create a separate
         * UNLINK event unless an explicit unlink succeeds, so the link
         * sequence is pretty useless */

        /* size must be a power of 2 to handle sequence # overflow */
        LASSERT (eq->eq_size != 0 &&
                 eq->eq_size == LOWEST_BIT_SET (eq->eq_size));
        eq_slot = eq->eq_events + (ev->sequence & (eq->eq_size - 1));

        /* There is no race since both event consumers and event producers
         * take the LNET_LOCK, so we don't screw around with memory
         * barriers, setting the sequence number last or wierd structure
         * layout assertions. */
        *eq_slot = *ev;

        /* Call the callback handler (if any) */
        if (eq->eq_callback != NULL)
                eq->eq_callback (eq_slot);

#ifdef __KERNEL__
        /* Wake anyone waiting in LNetEQPoll() */
        if (cfs_waitq_active(&the_lnet.ln_waitq))
                cfs_waitq_broadcast(&the_lnet.ln_waitq);
#else
# if LNET_SINGLE_THREADED
        /* LNetEQPoll() calls into _the_ LND to wait for action */
# else
        /* Wake anyone waiting in LNetEQPoll() */
        pthread_cond_broadcast(&the_lnet.ln_cond);
# endif
#endif
}

void
lnet_finalize (lnet_ni_t *ni, lnet_msg_t *msg, int status)
{
        lnet_handle_wire_t ack_wmd;
        lnet_libmd_t      *md;
        int                unlink;
        int                rc;

        LASSERT (!in_interrupt ());

        if (msg == NULL)
                return;

#if 0
        CDEBUG(D_WARNING, "%s msg->%s Flags:%s%s%s%s%s%s%s%s%s%s%s%s txp %s rxp %s\n",
               lnet_msgtyp2str(msg->msg_type), libcfs_id2str(msg->msg_target),
               msg->msg_target_is_router ? "t" : "",
               msg->msg_routing ? "X" : "",
               msg->msg_ack ? "A" : "",
               msg->msg_sending ? "S" : "",
               msg->msg_receiving ? "R" : "",
               msg->msg_recvaftersend ? "g" : "",
               msg->msg_delayed ? "d" : "",
               msg->msg_txcredit ? "C" : "",
               msg->msg_peertxcredit ? "c" : "",
               msg->msg_rtrcredit ? "F" : "",
               msg->msg_peerrtrcredit ? "f" : "",
               msg->msg_onactivelist ? "!" : "",
               msg->msg_txpeer == NULL ? "<none>" : libcfs_nid2str(msg->msg_txpeer->lp_nid),
               msg->msg_rxpeer == NULL ? "<none>" : libcfs_nid2str(msg->msg_rxpeer->lp_nid));
#endif
        LNET_LOCK();

        LASSERT (msg->msg_onactivelist);

        md = msg->msg_md;
        if (md != NULL) {
                /* Now it's safe to drop my caller's ref */
                md->md_pending--;
                LASSERT (md->md_pending >= 0);

                /* Should I unlink this MD? */
                if (md->md_pending != 0)        /* other refs */
                        unlink = 0;
                else if ((md->md_flags & LNET_MD_FLAG_ZOMBIE) != 0)
                        unlink = 1;
                else if ((md->md_flags & LNET_MD_FLAG_AUTO_UNLINK) == 0)
                        unlink = 0;
                else
                        unlink = lnet_md_exhausted(md);
                
                msg->msg_ev.status = status;
                msg->msg_ev.unlinked = unlink;
                
                if (md->md_eq != NULL)
                        lnet_enq_event_locked(md->md_eq, &msg->msg_ev);
                
                if (unlink)
                        lnet_md_unlink(md);

                msg->msg_md = NULL;
        }

        if (status == 0 && msg->msg_ack) {
                /* Only send an ACK if the PUT completed successfully */

                lnet_return_credits_locked(msg);

                msg->msg_ack = 0;
                LNET_UNLOCK();
        
                LASSERT(msg->msg_ev.type == LNET_EVENT_PUT);
                LASSERT(!msg->msg_routing);

                ack_wmd = msg->msg_hdr.msg.put.ack_wmd;
                
                lnet_prep_send(msg, LNET_MSG_ACK, msg->msg_ev.initiator, 0, 0);

                msg->msg_hdr.msg.ack.dst_wmd = ack_wmd;
                msg->msg_hdr.msg.ack.match_bits = msg->msg_ev.match_bits;
                msg->msg_hdr.msg.ack.mlength = cpu_to_le32(msg->msg_ev.mlength);
                
                LASSERT(!in_interrupt());
                rc = lnet_send(ni->ni_nid, msg);
                LASSERT(!in_interrupt());
                if (rc == 0)
                        return;

                LNET_LOCK();

        } else if (status == 0 &&               /* OK so far */
                   (msg->msg_routing && !msg->msg_sending)) { /* not forwarded */
                
                LASSERT (!msg->msg_receiving);  /* called back recv already */
        
                LNET_UNLOCK();
                
                LASSERT(!in_interrupt());
                rc = lnet_send(LNET_NID_ANY, msg);
                LASSERT(!in_interrupt());
                if (rc == 0)
                        return;

                LNET_LOCK();
        }

        lnet_return_credits_locked(msg);

        LASSERT (msg->msg_onactivelist);
        msg->msg_onactivelist = 0;
        list_del (&msg->msg_activelist);
        the_lnet.ln_counters.msgs_alloc--;
        lnet_msg_free(msg);
        
        LNET_UNLOCK();
}

