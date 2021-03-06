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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/fld/fld_internal.h
 *
 * Author: Yury Umanets <umka@clusterfs.com>
 * Author: Tom WangDi <wangdi@clusterfs.com>
 */
#ifndef __FLD_INTERNAL_H
#define __FLD_INTERNAL_H

#include <obd.h>
#include <lustre/lustre_idl.h>
#include <libcfs/libcfs.h>
#include <lustre_fld.h>

enum {
        LUSTRE_FLD_INIT = 1 << 0,
        LUSTRE_FLD_RUN  = 1 << 1
};

struct fld_stats {
        __u64   fst_count;
        __u64   fst_cache;
};

typedef int (*fld_hash_func_t) (struct lu_client_fld *, __u64);

typedef struct lu_fld_target *
(*fld_scan_func_t) (struct lu_client_fld *, __u64);

struct lu_fld_hash {
        const char              *fh_name;
        fld_hash_func_t          fh_hash_func;
        fld_scan_func_t          fh_scan_func;
};

struct fld_cache_entry {
        cfs_list_t               fce_lru;
        cfs_list_t               fce_list;
        /**
         * fld cache entries are sorted on range->lsr_start field. */
        struct lu_seq_range      fce_range;
};

struct fld_cache {
	/**
	 * Cache guard, protects fci_hash mostly because others immutable after
	 * init is finished.
	 */
	rwlock_t		 fci_lock;

        /**
         * Cache shrink threshold */
        int                      fci_threshold;

        /**
         * Prefered number of cached entries */
        int                      fci_cache_size;

        /**
         * Current number of cached entries. Protected by \a fci_lock */
        int                      fci_cache_count;

        /**
         * LRU list fld entries. */
        cfs_list_t               fci_lru;

        /**
         * sorted fld entries. */
        cfs_list_t               fci_entries_head;

        /**
         * Cache statistics. */
        struct fld_stats         fci_stat;

        /**
         * Cache name used for debug and messages. */
        char                     fci_name[80];
	unsigned int		 fci_no_shrink:1;
};

enum {
        /* 4M of FLD cache will not hurt client a lot. */
        FLD_SERVER_CACHE_SIZE      = (4 * 0x100000),

        /* 1M of FLD cache will not hurt client a lot. */
        FLD_CLIENT_CACHE_SIZE      = (1 * 0x100000)
};

enum {
        /* Cache threshold is 10 percent of size. */
        FLD_SERVER_CACHE_THRESHOLD = 10,

        /* Cache threshold is 10 percent of size. */
        FLD_CLIENT_CACHE_THRESHOLD = 10
};

extern struct lu_fld_hash fld_hash[];

#ifdef __KERNEL__

#ifdef LPROCFS
extern struct proc_dir_entry *fld_type_proc_dir;
extern struct lprocfs_seq_vars fld_client_proc_list[];
#endif

# ifdef HAVE_SERVER_SUPPORT
struct fld_thread_info {
	struct lu_seq_range fti_rec;
	struct lu_seq_range fti_lrange;
	struct lu_seq_range fti_irange;
};

extern struct lu_context_key fld_thread_key;

struct dt_device;
int fld_index_init(const struct lu_env *env, struct lu_server_fld *fld,
		   struct dt_device *dt);

void fld_index_fini(const struct lu_env *env, struct lu_server_fld *fld);

int fld_declare_index_create(const struct lu_env *env,
			     struct lu_server_fld *fld,
			     const struct lu_seq_range *new_range,
			     struct thandle *th);

int fld_index_create(const struct lu_env *env, struct lu_server_fld *fld,
		     const struct lu_seq_range *new_range, struct thandle *th);

int fld_index_lookup(const struct lu_env *env, struct lu_server_fld *fld,
		     seqno_t seq, struct lu_seq_range *range);

int fld_name_to_index(const char *name, __u32 *index);
int fld_server_mod_init(void);

void fld_server_mod_exit(void);

int fld_server_read(const struct lu_env *env, struct lu_server_fld *fld,
		    struct lu_seq_range *range, void *data, int data_len);
#ifdef LPROCFS
extern const struct file_operations fld_proc_seq_fops;
extern struct lprocfs_seq_vars fld_server_proc_list[];
#endif

# endif /* HAVE_SERVER_SUPPORT */

int fld_client_rpc(struct obd_export *exp,
                   struct lu_seq_range *range, __u32 fld_op,
		   struct ptlrpc_request **reqp);
#endif /* __KERNEL__ */

struct fld_cache *fld_cache_init(const char *name,
                                 int cache_size, int cache_threshold);

void fld_cache_fini(struct fld_cache *cache);

void fld_cache_flush(struct fld_cache *cache);

int fld_cache_insert(struct fld_cache *cache,
		     const struct lu_seq_range *range);

struct fld_cache_entry
*fld_cache_entry_create(const struct lu_seq_range *range);

int fld_cache_insert_nolock(struct fld_cache *cache,
			    struct fld_cache_entry *f_new);
void fld_cache_delete(struct fld_cache *cache,
                      const struct lu_seq_range *range);
void fld_cache_delete_nolock(struct fld_cache *cache,
			     const struct lu_seq_range *range);
int fld_cache_lookup(struct fld_cache *cache,
                     const seqno_t seq, struct lu_seq_range *range);

struct fld_cache_entry *
fld_cache_entry_lookup(struct fld_cache *cache,
		       const struct lu_seq_range *range);

void fld_cache_entry_delete(struct fld_cache *cache,
			    struct fld_cache_entry *node);

struct fld_cache_entry *
fld_cache_entry_lookup_nolock(struct fld_cache *cache,
			      const struct lu_seq_range *range);

static inline const char *
fld_target_name(const struct lu_fld_target *tar)
{
#ifdef HAVE_SERVER_SUPPORT
	if (tar->ft_srv != NULL)
		return tar->ft_srv->lsf_name;
#endif

	return tar->ft_exp->exp_obd->obd_name;
}

#endif /* __FLD_INTERNAL_H */
