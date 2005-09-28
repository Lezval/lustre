/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2005 Cluster File Systems, Inc. All rights reserved.
 *   Author: Eric Barton <eeb@bartonsoftware.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   This file is confidential source code owned by Cluster File Systems.
 *   No viewing, modification, compilation, redistribution, or any other
 *   form of use is permitted except through a signed license agreement.
 *
 *   If you have not signed such an agreement, then you have no rights to
 *   this file.  Please destroy it immediately and contact CFS.
 *
 */

#include "ptllnd.h"

lnd_t               the_ptllnd = {
        .lnd_type       = PTLLND,
        .lnd_startup    = ptllnd_startup,
        .lnd_shutdown   = ptllnd_shutdown,
        .lnd_send       = ptllnd_send,
        .lnd_recv       = ptllnd_recv,
        .lnd_eager_recv = ptllnd_eager_recv,
};

static int ptllnd_ni_count = 0;

int
ptllnd_parse_int_tunable(int *value, char *name, int dflt)
{
        char    *env = getenv(name);
        char    *end;

        if (env == NULL) {
                *value = dflt;
                return 0;
        }
        
        *value = strtoull(env, &end, 0);
        if (*end == 0)
                return 0;
        
        CERROR("Can't parse tunable %s=%s\n", name, env);
        return -EINVAL;
}

int
ptllnd_get_tunables(lnet_ni_t *ni)
{
        ptllnd_ni_t *plni = ni->ni_data;
        int          max_immediate;
        int          msgs_per_buffer;
        int          rc;

        rc = ptllnd_parse_int_tunable(&plni->plni_portal,
                                      "PTLLND_PORTAL", PTLLND_PORTAL);
        if (rc != 0)
                return rc;

        rc = ptllnd_parse_int_tunable(&plni->plni_pid,
                                      "PTLLND_PID", PTLLND_PID);
        if (rc != 0)
                return rc;

        rc = ptllnd_parse_int_tunable(&plni->plni_peer_credits,
                                      "PTLLND_PEERCREDITS", PTLLND_PEERCREDITS);
        if (rc != 0)
                return rc;

        rc = ptllnd_parse_int_tunable(&max_immediate,
                                      "PTLLND_MAX_MSG_SIZE", 
                                      PTLLND_MAX_MSG_SIZE);
        if (rc != 0)
                return rc;
        
        rc = ptllnd_parse_int_tunable(&msgs_per_buffer,
                                      "PTLLND_MSGS_PER_BUFFER", 
                                      PTLLND_MSGS_PER_BUFFER);
        if (rc != 0)
                return rc;

        rc = ptllnd_parse_int_tunable(&plni->plni_msgs_spare,
                                      "PTLLND_MSGS_SPARE", 
                                      PTLLND_MSGS_SPARE);
        if (rc != 0)
                return rc;

        rc = ptllnd_parse_int_tunable(&plni->plni_peer_hash_size,
                                      "PTLLND_PEER_HASH_SIZE", 
                                      PTLLND_PEER_HASH_SIZE);
        if (rc != 0)
                return rc;
        
        rc = ptllnd_parse_int_tunable(&plni->plni_eq_size,
                                      "PTLLND_EQ_SIZE", PTLLND_EQ_SIZE);
        if (rc != 0)
                return rc;

        plni->plni_max_msg_size =
                offsetof(kptl_msg_t, ptlm_u.immediate.kptlim_payload[max_immediate]);
        if (plni->plni_max_msg_size < sizeof(kptl_msg_t))
                plni->plni_max_msg_size = sizeof(kptl_msg_t);
        
        plni->plni_buffer_size = plni->plni_max_msg_size * msgs_per_buffer;
        return 0;
}

ptllnd_buffer_t *
ptllnd_create_buffer (lnet_ni_t *ni) 
{
        ptllnd_ni_t     *plni = ni->ni_data;
        ptllnd_buffer_t *buf;
        
        PORTAL_ALLOC(buf, sizeof(*buf));
        if (buf == NULL) {
                CERROR("Can't allocate buffer descriptor\n");
                return NULL;
        }

        buf->plb_ni = ni;
        buf->plb_posted = 0;
        
        PORTAL_ALLOC(buf->plb_buffer, plni->plni_buffer_size);
        if (buf->plb_buffer == NULL) {
                CERROR("Can't allocate buffer size %d\n",
                       plni->plni_buffer_size);
                PORTAL_FREE(buf, sizeof(*buf));
                return NULL;
        }
        
        list_add(&buf->plb_list, &plni->plni_buffers);
        plni->plni_nbuffers++;

        return buf;
}

void
ptllnd_destroy_buffer (ptllnd_buffer_t *buf)
{
        ptllnd_ni_t     *plni = buf->plb_ni->ni_data;

        LASSERT (!buf->plb_posted);

        plni->plni_nbuffers--;
        list_del(&buf->plb_list);
        PORTAL_FREE(buf->plb_buffer, plni->plni_buffer_size);
        PORTAL_FREE(buf, sizeof(*buf));
}

int
ptllnd_grow_buffers (lnet_ni_t *ni)
{
        ptllnd_ni_t     *plni = ni->ni_data;
        ptllnd_buffer_t *buf;
        int              nmsgs;
        int              nbufs;
        int              rc;
        
        nmsgs = plni->plni_npeers * plni->plni_peer_credits +
                plni->plni_msgs_spare;

        nbufs = (nmsgs * plni->plni_max_msg_size + plni->plni_buffer_size - 1) /
                plni->plni_buffer_size;
        
        while (nbufs > plni->plni_nbuffers) {
                buf = ptllnd_create_buffer(ni);
                
                if (buf == NULL)
                        return -ENOMEM;

                rc = ptllnd_post_buffer(buf);
                if (rc != 0)
                        return rc;
        }
        
        return 0;
}

void
ptllnd_destroy_buffers (lnet_ni_t *ni)
{
        ptllnd_ni_t       *plni = ni->ni_data;
        ptllnd_buffer_t   *buf;
        struct list_head  *tmp;
        struct list_head  *nxt;
        
        list_for_each_safe(tmp, nxt, &plni->plni_buffers) {
                buf = list_entry(tmp, ptllnd_buffer_t, plb_list);
                
                LASSERT (plni->plni_nbuffers > 0);
                if (buf->plb_posted) {
                        LASSERT (plni->plni_nposted_buffers > 0);

#ifdef LUSTRE_PORTALS_UNLINK_SEMANTICS                        
                        (void) PtlMDUnlink(buf->plb_md);
                        while (!buf->plb_posted)
                                ptllnd_wait(ni, -1);
#else
                        while (buf->plb_posted) {
                                rc = PtlMDUnlink(buf->plb_md);
                                if (rc == PTL_OK) {
                                        buf->plb_posted = 0;
                                        plni->plni_nposted_buffers--;
                                        break;
                                }
                                LASSERT (rc == PTL_MD_IN_USE);
                                ptllnd_wait(ni, -1);
                        }
#endif
                }
                ptllnd_destroy_buffer(buf);
        }

        LASSERT (plni->plni_nposted_buffers == 0);
        LASSERT (plni->plni_nbuffers == 0);
}

int
ptllnd_create_peer_hash (lnet_ni_t *ni)
{
        ptllnd_ni_t *plni = ni->ni_data;
        int          i;

        PORTAL_ALLOC(plni->plni_peer_hash, 
                     plni->plni_peer_hash_size * sizeof(*plni->plni_peer_hash));
        if (plni->plni_peer_hash == NULL) {
                CERROR("Can't allocate ptllnd peer hash (size %d)\n",
                       plni->plni_peer_hash_size);
                return -ENOMEM;
        }

        for (i = 0; i < plni->plni_peer_hash_size; i++)
                CFS_INIT_LIST_HEAD(&plni->plni_peer_hash[i]);
        
        return 0;
}

void 
ptllnd_destroy_peer_hash (lnet_ni_t *ni)
{
        ptllnd_ni_t    *plni = ni->ni_data;
        int             i;

        for (i = 0; i < plni->plni_peer_hash_size; i++)
                LASSERT (list_empty(&plni->plni_peer_hash[i]));
        
        PORTAL_FREE(plni->plni_peer_hash,
                    plni->plni_peer_hash_size * sizeof(*plni->plni_peer_hash));
}

void
ptllnd_close_peers (lnet_ni_t *ni)
{
        ptllnd_ni_t    *plni = ni->ni_data;
        ptllnd_peer_t  *plp;
        int             i;

        for (i = 0; i < plni->plni_peer_hash_size; i++)
                while (!list_empty(&plni->plni_peer_hash[i])) {
                        plp = list_entry(plni->plni_peer_hash[i].next,
                                         ptllnd_peer_t, plp_list);

                        ptllnd_close_peer(plp);
                }
}

__u64
ptllnd_get_timestamp(void)
{
        struct timeval  tv;
        int             rc = gettimeofday(&tv, NULL);

        LASSERT (rc == 0);
        return ((__u64)tv.tv_sec) * 1000000 + tv.tv_usec;
}

void 
ptllnd_shutdown (lnet_ni_t *ni)
{
        ptllnd_ni_t *plni = ni->ni_data;
        int          rc;

        LASSERT (ptllnd_ni_count == 1);

        ptllnd_destroy_buffers(ni);
        ptllnd_close_peers(ni);
        ptllnd_abort_txs(ni);

        while (plni->plni_npeers > 0)
                ptllnd_wait(ni, -1);

        LASSERT (plni->plni_ntxs == 0);
        LASSERT (plni->plni_nrxs == 0);
        
        rc = PtlEQFree(plni->plni_eqh);
        LASSERT (rc == PTL_OK);
        
        rc = PtlNIFini(plni->plni_nih);
        LASSERT (rc == PTL_OK);
        
        ptllnd_destroy_peer_hash(ni);
        PORTAL_FREE(plni, sizeof(*plni));
        ptllnd_ni_count--;
}

int
ptllnd_startup (lnet_ni_t *ni)
{
        ptllnd_ni_t *plni;
        int          rc;
        
	/* could get limits from portals I guess... */
	ni->ni_maxtxcredits = 
	ni->ni_peertxcredits = 1000;

        if (ptllnd_ni_count != 0) {
                CERROR("Can't have > 1 instance of ptllnd\n");
                return -EPERM;
        }

        ptllnd_ni_count++;
        
        PORTAL_ALLOC(plni, sizeof(*plni));
        if (plni == NULL) {
                CERROR("Can't allocate ptllnd state\n");
                rc = -ENOMEM;
                goto failed0;
        }
        
        ni->ni_data = plni;

        plni->plni_stamp = ptllnd_get_timestamp();
        plni->plni_nrxs = 0;
        plni->plni_ntxs = 0;
        CFS_INIT_LIST_HEAD(&plni->plni_active_txs);
        CFS_INIT_LIST_HEAD(&plni->plni_zombie_txs);

        rc = ptllnd_get_tunables(ni);
        if (rc != 0)
                goto failed1;

        rc = ptllnd_create_peer_hash(ni);
        if (rc != 0)
                goto failed1;

        rc = PtlNIInit(PTL_IFACE_DEFAULT, plni->plni_pid,
                       NULL, NULL, &plni->plni_nih);
        if (rc != PTL_OK && rc != PTL_IFACE_DUP) {
                CERROR("PtlNIInit failed: %d\n", rc);
                rc = -ENODEV;
                goto failed2;
        }

        rc = PtlEQAlloc(plni->plni_nih, plni->plni_eq_size,
                        PTL_EQ_HANDLER_NONE, &plni->plni_eqh);
        if (rc != PTL_OK) {
                CERROR("PtlEQAlloc failed: %d\n", rc);
                rc = -ENODEV;
                goto failed3;
        }

        rc = ptllnd_grow_buffers(ni);
        if (rc != 0)
                goto failed4;

	return 0;

 failed4:
        ptllnd_destroy_buffers(ni);
        PtlEQFree(plni->plni_eqh);
 failed3:
        PtlNIFini(plni->plni_nih);
 failed2:
        ptllnd_destroy_peer_hash(ni);
 failed1:
        PORTAL_FREE(plni, sizeof(*plni));
 failed0:
        ptllnd_ni_count--;
        return rc;
}

