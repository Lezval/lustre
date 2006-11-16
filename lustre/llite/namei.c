/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
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

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/quotaops.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include <obd_support.h>
#include <lustre_fid.h>
#include <lustre_lite.h>
#include <lustre_dlm.h>
#include <lustre_ver.h>
#include <lustre_mdc.h>
#include "llite_internal.h"

/* methods */

extern struct dentry_operations ll_d_ops;

int ll_unlock(__u32 mode, struct lustre_handle *lockh)
{
        ENTRY;

        ldlm_lock_decref(lockh, mode);

        RETURN(0);
}

/*
 * Get an inode by inode number (already instantiated by the intent lookup).
 * Returns inode or NULL
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
struct inode *ll_iget(struct super_block *sb, ino_t hash,
                      struct lustre_md *md)
{
        struct ll_inode_info *lli;
        struct inode *inode;
        LASSERT(hash != 0);

        inode = iget_locked(sb, hash);
        if (inode) {
                if (inode->i_state & I_NEW) {
                        lli = ll_i2info(inode);
                        ll_read_inode2(inode, md);
                        unlock_new_inode(inode);
                } else {
                        if (!(inode->i_state & (I_FREEING | I_CLEAR)))
                                ll_update_inode(inode, md);
                }
                CDEBUG(D_VFSTRACE, "inode: %lu/%u(%p)\n",
                       inode->i_ino, inode->i_generation, inode);
        }

        return inode;
}
#else
struct inode *ll_iget(struct super_block *sb, ino_t hash,
                      struct lustre_md *md)
{
        struct inode *inode;
        LASSERT(hash != 0);

        inode = iget4(sb, hash, NULL, md);
        if (inode) {
                if (!(inode->i_state & (I_FREEING | I_CLEAR)))
                        ll_update_inode(inode, md);

                CDEBUG(D_VFSTRACE, "inode: %lu/%u(%p)\n",
                       inode->i_ino, inode->i_generation, inode);
        }
        return inode;
}
#endif

int ll_md_blocking_ast(struct ldlm_lock *lock, struct ldlm_lock_desc *desc,
                       void *data, int flag)
{
        int rc;
        struct lustre_handle lockh;
        ENTRY;

        switch (flag) {
        case LDLM_CB_BLOCKING:
                ldlm_lock2handle(lock, &lockh);
                rc = ldlm_cli_cancel(&lockh);
                if (rc < 0) {
                        CDEBUG(D_INODE, "ldlm_cli_cancel: %d\n", rc);
                        RETURN(rc);
                }
                break;
        case LDLM_CB_CANCELING: {
                struct inode *inode = ll_inode_from_lock(lock);
                __u64 bits = lock->l_policy_data.l_inodebits.bits;
                struct lu_fid *fid;

                /* Invalidate all dentries associated with this inode */
                if (inode == NULL)
                        break;

                fid = ll_inode2fid(inode);
                if (lock->l_resource->lr_name.name[0] != fid_seq(fid) ||
                    lock->l_resource->lr_name.name[1] != fid_oid(fid) ||
                    lock->l_resource->lr_name.name[2] != fid_ver(fid)) {
                        LDLM_ERROR(lock, "data mismatch with object "
                                   DFID" (%p)", PFID(fid), inode);
                }

                if (bits & MDS_INODELOCK_OPEN) {
                        int flags = 0;
                        switch (lock->l_req_mode) {
                        case LCK_CW:
                                flags = FMODE_WRITE;
                                break;
                        case LCK_PR:
                                flags = FMODE_EXEC;
                                if (!FMODE_EXEC)
                                        CERROR("open PR lock without FMODE_EXEC\n");
                                break;
                        case LCK_CR:
                                flags = FMODE_READ;
                                break;
                        default:
                                CERROR("Unexpected lock mode for OPEN lock "
                                       "%d, inode %ld\n", lock->l_req_mode,
                                       inode->i_ino);
                        }
                        ll_md_real_close(inode, flags);
                }

                if (bits & MDS_INODELOCK_UPDATE)
                        ll_i2info(inode)->lli_flags &= ~LLIF_MDS_SIZE_LOCK;

                if (S_ISDIR(inode->i_mode) &&
                     (bits & MDS_INODELOCK_UPDATE)) {
                        struct dentry *dentry, *tmp, *dir;
                        struct list_head *list;
                        
                        CDEBUG(D_INODE, "invalidating inode %lu\n",
                               inode->i_ino);
                        truncate_inode_pages(inode->i_mapping, 0);

                        
                        /* Drop possible cached negative dentries */
                        list = &inode->i_dentry;
                        dir = NULL;
                        spin_lock(&dcache_lock);
                        
                        /* It is possible to have several dentries (with 
                           racer?) */
                        while ((list = list->next) != &inode->i_dentry) {
                                dir = list_entry(list, struct dentry, d_alias);
#ifdef LUSTRE_KERNEL_VERSION
                                if (!(dir->d_flags & DCACHE_LUSTRE_INVALID))
#else
                                if (!d_unhashed(dir))
#endif
                                        break;

                                dir = NULL;
                        }
                        
                        if (dir) {
restart:
                                list_for_each_entry_safe(dentry, tmp, 
                                                         &dir->d_subdirs, 
                                                         d_child)
                                {
                                        /* XXX Print some debug here? */
                                        if (!dentry->d_inode) 
                                                /* Negative dentry. If we were 
                                                   dropping dcache lock, go 
                                                   throught the list again */
                                                if (ll_drop_dentry(dentry))
                                                        goto restart;
                                }
                        }
                        spin_unlock(&dcache_lock);
                }

                if (inode->i_sb->s_root &&
                    inode != inode->i_sb->s_root->d_inode &&
                    (bits & MDS_INODELOCK_LOOKUP))
                        ll_unhash_aliases(inode);
                iput(inode);
                break;
        }
        default:
                LBUG();
        }

        RETURN(0);
}

/* Pack the required supplementary groups into the supplied groups array.
 * If we don't need to use the groups from the target inode(s) then we
 * instead pack one or more groups from the user's supplementary group
 * array in case it might be useful.  Not needed if doing an MDS-side upcall. */
void ll_i2gids(__u32 *suppgids, struct inode *i1, struct inode *i2)
{
        int i;

        LASSERT(i1 != NULL);
        LASSERT(suppgids != NULL);

        if (in_group_p(i1->i_gid))
                suppgids[0] = i1->i_gid;
        else
                suppgids[0] = -1;

        if (i2) {
                if (in_group_p(i2->i_gid))
                        suppgids[1] = i2->i_gid;
                else
                        suppgids[1] = -1;
        } else {
                suppgids[1] = -1;
        }

        for (i = 0; i < current_ngroups; i++) {
                if (suppgids[0] == -1) {
                        if (current_groups[i] != suppgids[1])
                                suppgids[0] = current_groups[i];
                        continue;
                }
                if (suppgids[1] == -1) {
                        if (current_groups[i] != suppgids[0])
                                suppgids[1] = current_groups[i];
                        continue;
                }
                break;
        }
}

static void ll_d_add(struct dentry *de, struct inode *inode)
{
        CDEBUG(D_DENTRY, "adding inode %p to dentry %p\n", inode, de);
        /* d_instantiate */
        if (!list_empty(&de->d_alias)) {
                spin_unlock(&dcache_lock);
                CERROR("dentry %.*s %p alias next %p, prev %p\n",
                       de->d_name.len, de->d_name.name, de,
                       de->d_alias.next, de->d_alias.prev);
                LBUG();
        }
        if (inode)
                list_add(&de->d_alias, &inode->i_dentry);
        de->d_inode = inode;

        /* d_rehash */
        if (!d_unhashed(de)) {
                spin_unlock(&dcache_lock);
                CERROR("dentry %.*s %p hash next %p\n",
                       de->d_name.len, de->d_name.name, de, de->d_hash.next);
                LBUG();
        }
        __d_rehash(de, 0);
}

/* 2.6.15 and prior versions have buggy d_instantiate_unique that leaks an inode
 * if suitable alias is found. But we are not going to fix it by just freeing
 * such inode, because if some vendor's kernel contains this bugfix already,
 * we will break everything then. We will use our own reimplementation
 * instead. */
#if !defined(HAVE_D_ADD_UNIQUE) || (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16))
/* Search "inode"'s alias list for a dentry that has the same name and parent as
 * de.  If found, return it.  If not found, return de. */
struct dentry *ll_find_alias(struct inode *inode, struct dentry *de)
{
        struct list_head *tmp;

        spin_lock(&dcache_lock);
        list_for_each(tmp, &inode->i_dentry) {
                struct dentry *dentry = list_entry(tmp, struct dentry, d_alias);

                /* We are called here with 'de' already on the aliases list. */
                if (dentry == de) {
                        CERROR("whoops\n");
                        continue;
                }

                if (dentry->d_parent != de->d_parent)
                        continue;

                if (dentry->d_name.len != de->d_name.len)
                        continue;

                if (memcmp(dentry->d_name.name, de->d_name.name,
                           de->d_name.len) != 0)
                        continue;

                dget_locked(dentry);
                lock_dentry(dentry);
                __d_drop(dentry);
#ifdef LUSTRE_KERNEL_VERSION
                dentry->d_flags &= ~DCACHE_LUSTRE_INVALID;
#endif
                unlock_dentry(dentry);
                __d_rehash(dentry, 0); /* avoid taking dcache_lock inside */
                spin_unlock(&dcache_lock);
                iput(inode);
                CDEBUG(D_DENTRY, "alias dentry %.*s (%p) parent %p inode %p "
                       "refc %d\n", de->d_name.len, de->d_name.name, de,
                       de->d_parent, de->d_inode, atomic_read(&de->d_count));
                return dentry;
        }

        ll_d_add(de, inode);

        spin_unlock(&dcache_lock);

        return de;
}
#else
struct dentry *ll_find_alias(struct inode *inode, struct dentry *de)
{
        struct dentry *dentry;

        dentry = d_add_unique(de, inode);
#ifdef LUSTRE_KERNEL_VERSION
        if (dentry) {
                lock_dentry(dentry);
                dentry->d_flags &= ~DCACHE_LUSTRE_INVALID;
                unlock_dentry(dentry);
        }
#endif

        return dentry?dentry:de;
}
#endif

static int lookup_it_finish(struct ptlrpc_request *request, int offset,
                            struct lookup_intent *it, void *data)
{
        struct it_cb_data *icbd = data;
        struct dentry **de = icbd->icbd_childp;
        struct inode *parent = icbd->icbd_parent;
        struct ll_sb_info *sbi = ll_i2sbi(parent);
        struct inode *inode = NULL;
        int rc;

        /* NB 1 request reference will be taken away by ll_intent_lock()
         * when I return */
        if (!it_disposition(it, DISP_LOOKUP_NEG)) {
                ENTRY;

                rc = ll_prep_inode(&inode, request, offset,
                                   (*de)->d_sb);
                if (rc)
                        RETURN(rc);

                CDEBUG(D_DLMTRACE, "setting l_data to inode %p (%lu/%u)\n",
                       inode, inode->i_ino, inode->i_generation);
                md_set_lock_data(sbi->ll_md_exp,
                                 &it->d.lustre.it_lock_handle, inode);

                /* We used to query real size from OSTs here, but actually
                   this is not needed. For stat() calls size would be updated
                   from subsequent do_revalidate()->ll_inode_revalidate_it() in
                   2.4 and
                   vfs_getattr_it->ll_getattr()->ll_inode_revalidate_it() in 2.6
                   Everybody else who needs correct file size would call
                   ll_glimpse_size or some equivalent themselves anyway.
                   Also see bug 7198. */

                *de = ll_find_alias(inode, *de);
        } else {
                ENTRY;
                /* Check that parent has UPDATE lock. If there is none, we
                   cannot afford to hash this dentry (done by ll_d_add) as it
                   might get picked up later when UPDATE lock will appear */
                if (ll_have_md_lock(parent, MDS_INODELOCK_UPDATE)) {
                        spin_lock(&dcache_lock);
                        ll_d_add(*de, inode);
                        spin_unlock(&dcache_lock);
                } else {
                        (*de)->d_inode = NULL; 
                }
        }

        ll_set_dd(*de);
        (*de)->d_op = &ll_d_ops;

        RETURN(0);
}

static struct dentry *ll_lookup_it(struct inode *parent, struct dentry *dentry,
                                   struct lookup_intent *it, int lookup_flags)
{
        struct lookup_intent lookup_it = { .it_op = IT_LOOKUP };
        struct dentry *save = dentry, *retval;
        struct ptlrpc_request *req = NULL;
        struct md_op_data *op_data;
        struct it_cb_data icbd;
        __u32 opc;
        int rc;
        ENTRY;

        if (dentry->d_name.len > ll_i2sbi(parent)->ll_namelen)
                RETURN(ERR_PTR(-ENAMETOOLONG));

        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p),intent=%s\n",
               dentry->d_name.len, dentry->d_name.name, parent->i_ino,
               parent->i_generation, parent, LL_IT2STR(it));

        if (d_mountpoint(dentry))
                CERROR("Tell Peter, lookup on mtpt, it %s\n", LL_IT2STR(it));

        ll_frob_intent(&it, &lookup_it);

        icbd.icbd_childp = &dentry;
        icbd.icbd_parent = parent;

        if (it->it_op & IT_CREAT ||
            (it->it_op & IT_OPEN && it->it_create_mode & O_CREAT))
                opc = LUSTRE_OPC_CREATE;
        else
                opc = LUSTRE_OPC_ANY;
        
        op_data = ll_prep_md_op_data(NULL, parent, NULL, dentry->d_name.name,
                                     dentry->d_name.len, lookup_flags, opc);
        if (op_data == NULL)
                RETURN(ERR_PTR(-ENOMEM));

        it->it_create_mode &= ~current->fs->umask;

        rc = md_intent_lock(ll_i2mdexp(parent), op_data, NULL, 0, it,
                            lookup_flags, &req, ll_md_blocking_ast, 0);
        ll_finish_md_op_data(op_data);
        if (rc < 0)
                GOTO(out, retval = ERR_PTR(rc));

        rc = lookup_it_finish(req, DLM_REPLY_REC_OFF, it, &icbd);
        if (rc != 0) {
                ll_intent_release(it);
                GOTO(out, retval = ERR_PTR(rc));
        }

        if ((it->it_op & IT_OPEN) && dentry->d_inode &&
            !S_ISREG(dentry->d_inode->i_mode) &&
            !S_ISDIR(dentry->d_inode->i_mode)) {
                ll_release_openhandle(dentry, it);
        }
        ll_lookup_finish_locks(it, dentry);

        if (dentry == save)
                GOTO(out, retval = NULL);
        else
                GOTO(out, retval = dentry);
 out:
        if (req)
                ptlrpc_req_finished(req);
        return retval;
}

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
#ifdef LUSTRE_KERNEL_VERSION
static struct dentry *ll_lookup_nd(struct inode *parent, struct dentry *dentry,
                                   struct nameidata *nd)
{
        struct dentry *de;
        ENTRY;

        if (nd && nd->flags & LOOKUP_LAST && !(nd->flags & LOOKUP_LINK_NOTLAST))
                de = ll_lookup_it(parent, dentry, &nd->intent, nd->flags);
        else
                de = ll_lookup_it(parent, dentry, NULL, 0);

        RETURN(de);
}
#else
struct lookup_intent *ll_convert_intent(struct open_intent *oit,
                                        int lookup_flags)
{
        struct lookup_intent *it;

        OBD_ALLOC(it, sizeof(*it));
        if (!it)
                return ERR_PTR(-ENOMEM);

        if (lookup_flags & LOOKUP_OPEN) {
                it->it_op = IT_OPEN;
                if (lookup_flags & LOOKUP_CREATE)
                        it->it_op |= IT_CREAT;
                it->it_create_mode = oit->create_mode;
                it->it_flags = oit->flags;
        } else {
                it->it_op = IT_GETATTR;
        }

#ifndef HAVE_FILE_IN_STRUCT_INTENT
                /* Since there is no way to pass our intent to ll_file_open,
                 * just check the file is there. Actual open will be done
                 * in ll_file_open */
                if (it->it_op & IT_OPEN)
                        it->it_op = IT_LOOKUP;
#endif

        return it;
}

static struct dentry *ll_lookup_nd(struct inode *parent, struct dentry *dentry,
                                   struct nameidata *nd)
{
        struct dentry *de;
        ENTRY;

        if (nd && !(nd->flags & (LOOKUP_CONTINUE|LOOKUP_PARENT))) {
                struct lookup_intent *it;

#if defined(HAVE_FILE_IN_STRUCT_INTENT) && (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17))
                /* Did we came here from failed revalidate just to propagate
                 * its error? */
                if (nd->flags & LOOKUP_OPEN)
                        if (IS_ERR(nd->intent.open.file))
                                RETURN((struct dentry *)nd->intent.open.file);
#endif

                if (ll_d2d(dentry) && ll_d2d(dentry)->lld_it) {
                        it = ll_d2d(dentry)->lld_it;
                        ll_d2d(dentry)->lld_it = NULL;
                } else {
                        it = ll_convert_intent(&nd->intent.open, nd->flags);
                        if (IS_ERR(it))
                                RETURN((struct dentry *)it);
                }

                de = ll_lookup_it(parent, dentry, it, nd->flags);
                if (de)
                        dentry = de;
                if ((nd->flags & LOOKUP_OPEN) && !IS_ERR(dentry)) { /* Open */
                        if (dentry->d_inode && 
                            it_disposition(it, DISP_OPEN_OPEN)) { /* nocreate */
#ifdef HAVE_FILE_IN_STRUCT_INTENT
                                if (S_ISFIFO(dentry->d_inode->i_mode)) {
                                        // We cannot call open here as it would
                                        // deadlock.
                                        ptlrpc_req_finished(
                                                       (struct ptlrpc_request *)
                                                          it->d.lustre.it_data);
                                } else {
                                        struct file *filp;
                                        nd->intent.open.file->private_data = it;
                                        filp =lookup_instantiate_filp(nd,dentry,
                                                                      NULL);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17))
/* 2.6.1[456] have a bug in open_namei() that forgets to check
 * nd->intent.open.file for error, so we need to return it as lookup's result
 * instead */
                                        if (IS_ERR(filp)) {
                                                if (de)
                                                        dput(de);
                                                de = (struct dentry *) filp;
                                        }
#endif
                                                
                                }
#else /* HAVE_FILE_IN_STRUCT_INTENT */
                                /* Release open handle as we have no way to
                                 * pass it to ll_file_open */
                                ll_release_openhandle(dentry, it);
#endif /* HAVE_FILE_IN_STRUCT_INTENT */
                        } else if (it_disposition(it, DISP_OPEN_CREATE)) {
                                // XXX This can only reliably work on assumption
                                // that there are NO hashed negative dentries.
                                ll_d2d(dentry)->lld_it = it;
                                it = NULL; /* Will be freed in ll_create_nd */
                                /* We absolutely depend on ll_create_nd to be
                                 * called to not leak this intent and possible
                                 * data attached to it */
                        }
                }

                if (it) {
                        ll_intent_release(it);
                        OBD_FREE(it, sizeof(*it));
                }
        } else {
                de = ll_lookup_it(parent, dentry, NULL, 0);
        }

        RETURN(de);
}
#endif
#endif

/* We depend on "mode" being set with the proper file type/umask by now */
static struct inode *ll_create_node(struct inode *dir, const char *name,
                                    int namelen, const void *data, int datalen,
                                    int mode, __u64 extra,
                                    struct lookup_intent *it)
{
        struct inode *inode = NULL;
        struct ptlrpc_request *request = NULL;
        struct ll_sb_info *sbi = ll_i2sbi(dir);
        int rc;
        ENTRY;

        LASSERT(it && it->d.lustre.it_disposition);

        LASSERT(it_disposition(it, DISP_ENQ_CREATE_REF));
        request = it->d.lustre.it_data;
        it_clear_disposition(it, DISP_ENQ_CREATE_REF);
        rc = ll_prep_inode(&inode, request, DLM_REPLY_REC_OFF, dir->i_sb);
        if (rc)
                GOTO(out, inode = ERR_PTR(rc));

        LASSERT(list_empty(&inode->i_dentry));

        /* We asked for a lock on the directory, but were granted a
         * lock on the inode.  Since we finally have an inode pointer,
         * stuff it in the lock. */
        CDEBUG(D_DLMTRACE, "setting l_ast_data to inode %p (%lu/%u)\n",
               inode, inode->i_ino, inode->i_generation);
        md_set_lock_data(sbi->ll_md_exp,
                         &it->d.lustre.it_lock_handle, inode);
        EXIT;
 out:
        ptlrpc_req_finished(request);
        return inode;
}

/*
 * By the time this is called, we already have created the directory cache
 * entry for the new file, but it is so far negative - it has no inode.
 *
 * We defer creating the OBD object(s) until open, to keep the intent and
 * non-intent code paths similar, and also because we do not have the MDS
 * inode number before calling ll_create_node() (which is needed for LOV),
 * so we would need to do yet another RPC to the MDS to store the LOV EA
 * data on the MDS.  If needed, we would pass the PACKED lmm as data and
 * lmm_size in datalen (the MDS still has code which will handle that).
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate().
 */
static int ll_create_it(struct inode *dir, struct dentry *dentry, int mode,
                        struct lookup_intent *it)
{
        struct inode *inode;
        int rc = 0;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p),intent=%s\n",
               dentry->d_name.len, dentry->d_name.name, dir->i_ino,
               dir->i_generation, dir, LL_IT2STR(it));

        rc = it_open_error(DISP_OPEN_CREATE, it);
        if (rc)
                RETURN(rc);

        inode = ll_create_node(dir, dentry->d_name.name, dentry->d_name.len,
                               NULL, 0, mode, 0, it);
        if (IS_ERR(inode)) {
                RETURN(PTR_ERR(inode));
        }

        /* Negative dentry may be unhashed if parent does not have UPDATE lock,
         * but some callers, e.g. do_coredump, expect dentry to be hashed after
         * successful create. Hash it here. */
        spin_lock(&dcache_lock);
        ll_d_add(dentry, inode);
        spin_unlock(&dcache_lock);
        RETURN(0);
}

static void ll_update_times(struct ptlrpc_request *request, int offset,
                            struct inode *inode)
{
        struct mdt_body *body = lustre_msg_buf(request->rq_repmsg, offset,
                                               sizeof(*body));
        LASSERT(body);

        /* mtime is always updated with ctime, but can be set in past.
           As write and utime(2) may happen within 1 second, and utime's
           mtime has a priority over write's one, so take mtime from mds 
           for the same ctimes. */
        if (body->valid & OBD_MD_FLCTIME &&
            body->ctime >= LTIME_S(inode->i_ctime)) {
                LTIME_S(inode->i_ctime) = body->ctime;

                if (body->valid & OBD_MD_FLMTIME) {
                        CDEBUG(D_INODE, "setting ino %lu mtime from %lu "
                               "to "LPU64"\n", inode->i_ino, 
                               LTIME_S(inode->i_mtime), body->mtime);
                        LTIME_S(inode->i_mtime) = body->mtime;
                }
        }
}

static int ll_mknod_generic(struct inode *dir, struct qstr *name, int mode,
                            unsigned rdev, struct dentry *dchild)
{
        struct ll_sb_info *sbi = ll_i2sbi(dir);
        struct ptlrpc_request *request = NULL;
        struct md_op_data *op_data;
        struct inode *inode = NULL;
        int err;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p) mode %o dev %x\n",
               name->len, name->name, dir->i_ino, dir->i_generation, dir,
               mode, rdev);

        mode &= ~current->fs->umask;

        switch (mode & S_IFMT) {
        case 0:
        case S_IFREG:
                mode |= S_IFREG; /* for mode = 0 case, fallthrough */
        case S_IFCHR:
        case S_IFBLK:
        case S_IFIFO:
        case S_IFSOCK:
                op_data = ll_prep_md_op_data(NULL, dir, NULL, name->name,
                                             name->len, 0, LUSTRE_OPC_MKNOD);
                if (op_data == NULL)
                        RETURN(-ENOMEM);

                err = md_create(sbi->ll_md_exp, op_data, NULL, 0, mode,
                                current->fsuid, current->fsgid,
                                current->cap_effective, rdev, &request);
                ll_finish_md_op_data(op_data);
                if (err)
                        break;
                ll_update_times(request, REPLY_REC_OFF, dir);

                if (dchild) {
                        err = ll_prep_inode(&inode, request, REPLY_REC_OFF,
                                            dchild->d_sb);
                        if (err)
                                break;

                        d_instantiate(dchild, inode);
                }
                break;
        case S_IFDIR:
                err = -EPERM;
                break;
        default:
                err = -EINVAL;
        }
        ptlrpc_req_finished(request);
        RETURN(err);
}

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
#ifndef LUSTRE_KERNEL_VERSION
static int ll_create_nd(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{
        struct lookup_intent *it = ll_d2d(dentry)->lld_it;
        int rc;
        
        if (!it)
                return ll_mknod_generic(dir, &dentry->d_name, mode, 0, dentry);
                
        ll_d2d(dentry)->lld_it = NULL;
        
        /* Was there an error? Propagate it! */
        if (it->d.lustre.it_status) {
                rc = it->d.lustre.it_status;
                goto out;
        }       
        
        rc = ll_create_it(dir, dentry, mode, it);
#ifdef HAVE_FILE_IN_STRUCT_INTENT
        if (nd && (nd->flags & LOOKUP_OPEN) && dentry->d_inode) { /* Open */
                nd->intent.open.file->private_data = it;
                lookup_instantiate_filp(nd, dentry, NULL);
        }
#else
        ll_release_openhandle(dentry,it);
#endif

out:
        ll_intent_release(it);
        OBD_FREE(it, sizeof(*it));

        return rc;
}
#else
static int ll_create_nd(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{
        if (!nd || !nd->intent.d.lustre.it_disposition)
                /* No saved request? Just mknod the file */
                return ll_mknod_generic(dir, &dentry->d_name, mode, 0, dentry);

        return ll_create_it(dir, dentry, mode, &nd->intent);
}
#endif
#endif

static int ll_symlink_generic(struct inode *dir, struct dentry *dchild,
                              const char *tgt)
{
        struct qstr *name = &dchild->d_name;
        struct ptlrpc_request *request = NULL;
        struct ll_sb_info *sbi = ll_i2sbi(dir);
        struct inode *inode = NULL;
        struct md_op_data *op_data;
        int err;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p),target=%s\n",
               name->len, name->name, dir->i_ino, dir->i_generation,
               dir, tgt);

        op_data = ll_prep_md_op_data(NULL, dir, NULL, name->name,
                                     name->len, 0, LUSTRE_OPC_SYMLINK);
        if (op_data == NULL)
                RETURN(-ENOMEM);

        err = md_create(sbi->ll_md_exp, op_data, tgt, strlen(tgt) + 1,
                        S_IFLNK | S_IRWXUGO, current->fsuid, current->fsgid,
                        current->cap_effective, 0, &request);
        ll_finish_md_op_data(op_data);
        if (err == 0) {
                ll_update_times(request, REPLY_REC_OFF, dir);

                if (dchild) {
                        err = ll_prep_inode(&inode, request, REPLY_REC_OFF,
                                            dchild->d_sb);
                        if (err == 0)
                                d_instantiate(dchild, inode);
                }
        }

        ptlrpc_req_finished(request);
        RETURN(err);
}

static int ll_link_generic(struct inode *src,  struct inode *dir,
                           struct qstr *name)
{
        struct ll_sb_info *sbi = ll_i2sbi(dir);
        struct ptlrpc_request *request = NULL;
        struct md_op_data *op_data;
        int err;

        ENTRY;
        CDEBUG(D_VFSTRACE,
               "VFS Op: inode=%lu/%u(%p), dir=%lu/%u(%p), target=%.*s\n",
               src->i_ino, src->i_generation, src, dir->i_ino,
               dir->i_generation, dir, name->len, name->name);

        op_data = ll_prep_md_op_data(NULL, src, dir, name->name, name->len, 
                                     0, LUSTRE_OPC_ANY);
        if (op_data == NULL)
                RETURN(-ENOMEM);
        err = md_link(sbi->ll_md_exp, op_data, &request);
        ll_finish_md_op_data(op_data);
        if (err == 0)
                ll_update_times(request, REPLY_REC_OFF, dir);

        ptlrpc_req_finished(request);
        RETURN(err);
}

static int ll_mkdir_generic(struct inode *dir, struct qstr *name,
                            int mode, struct dentry *dchild)

{
        struct ptlrpc_request *request = NULL;
        struct ll_sb_info *sbi = ll_i2sbi(dir);
        struct inode *inode = NULL;
        struct md_op_data *op_data;
        int err;
        ENTRY;
        
        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p)\n",
               name->len, name->name, dir->i_ino, dir->i_generation, dir);

        mode = (mode & (S_IRWXUGO|S_ISVTX) & ~current->fs->umask) | S_IFDIR;

        op_data = ll_prep_md_op_data(NULL, dir, NULL, name->name, name->len,
                                     0, LUSTRE_OPC_MKDIR);
        if (op_data == NULL)
                RETURN(-ENOMEM);

        err = md_create(sbi->ll_md_exp, op_data, NULL, 0, mode,
                        current->fsuid, current->fsgid,
                        current->cap_effective, 0, &request);
        
        ll_finish_md_op_data(op_data);
        if (err == 0) {
                ll_update_times(request, REPLY_REC_OFF, dir);
                if (dchild) {
                        err = ll_prep_inode(&inode, request, REPLY_REC_OFF,
                                            dchild->d_sb);
                        if (err == 0)
                                d_instantiate(dchild, inode);
                }
        }
        if (request != NULL)
                ptlrpc_req_finished(request);
        RETURN(err);
}

static int ll_rmdir_generic(struct inode *dir, struct dentry *dparent,
                            struct qstr *name)
{
        struct ptlrpc_request *request = NULL;
        struct md_op_data *op_data;
        struct dentry *dentry;
        int rc;
        ENTRY;
        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p)\n",
               name->len, name->name, dir->i_ino, dir->i_generation, dir);

        /* Check if we have something mounted at the dir we are going to delete
         * In such a case there would always be dentry present. */
        if (dparent) {
                dentry = d_lookup(dparent, name);
                if (dentry) {
                        int mounted = d_mountpoint(dentry);
                        dput(dentry);
                        if (mounted)
                                RETURN(-EBUSY);
                }
        }

        op_data = ll_prep_md_op_data(NULL, dir, NULL, name->name, name->len,
                                     S_IFDIR, LUSTRE_OPC_ANY);
        if (op_data == NULL)
                RETURN(-ENOMEM);
        rc = md_unlink(ll_i2sbi(dir)->ll_md_exp, op_data, &request);
        ll_finish_md_op_data(op_data);
        if (rc == 0)
                ll_update_times(request, REPLY_REC_OFF, dir);
        ptlrpc_req_finished(request);
        RETURN(rc);
}

int ll_objects_destroy(struct ptlrpc_request *request, struct inode *dir)
{
        struct mdt_body *body;
        struct lov_mds_md *eadata;
        struct lov_stripe_md *lsm = NULL;
        struct obd_trans_info oti = { 0 };
        struct obdo *oa;
        int rc;
        ENTRY;

        /* req is swabbed so this is safe */
        body = lustre_msg_buf(request->rq_repmsg, REPLY_REC_OFF, sizeof(*body));

        if (!(body->valid & OBD_MD_FLEASIZE))
                RETURN(0);

        if (body->eadatasize == 0) {
                CERROR("OBD_MD_FLEASIZE set but eadatasize zero\n");
                GOTO(out, rc = -EPROTO);
        }

        /* The MDS sent back the EA because we unlinked the last reference
         * to this file. Use this EA to unlink the objects on the OST.
         * It's opaque so we don't swab here; we leave it to obd_unpackmd() to
         * check it is complete and sensible. */
        eadata = lustre_swab_repbuf(request, REPLY_REC_OFF + 1,
                                    body->eadatasize, NULL);
        LASSERT(eadata != NULL);
        if (eadata == NULL) {
                CERROR("Can't unpack MDS EA data\n");
                GOTO(out, rc = -EPROTO);
        }

        rc = obd_unpackmd(ll_i2dtexp(dir), &lsm, eadata, body->eadatasize);
        if (rc < 0) {
                CERROR("obd_unpackmd: %d\n", rc);
                GOTO(out, rc);
        }
        LASSERT(rc >= sizeof(*lsm));

        rc = obd_checkmd(ll_i2dtexp(dir), ll_i2mdexp(dir), lsm);
        if (rc)
                GOTO(out_free_memmd, rc);

        oa = obdo_alloc();
        if (oa == NULL)
                GOTO(out_free_memmd, rc = -ENOMEM);

        oa->o_id = lsm->lsm_object_id;
        oa->o_gr = lsm->lsm_object_gr;
        oa->o_mode = body->mode & S_IFMT;
        oa->o_valid = OBD_MD_FLID | OBD_MD_FLTYPE | OBD_MD_FLGROUP;

        if (body->valid & OBD_MD_FLCOOKIE) {
                oa->o_valid |= OBD_MD_FLCOOKIE;
                oti.oti_logcookies =
                        lustre_msg_buf(request->rq_repmsg, REPLY_REC_OFF + 2,
                                       sizeof(struct llog_cookie) *
                                       lsm->lsm_stripe_count);
                if (oti.oti_logcookies == NULL) {
                        oa->o_valid &= ~OBD_MD_FLCOOKIE;
                        body->valid &= ~OBD_MD_FLCOOKIE;
                }
        }

        rc = obd_destroy(ll_i2dtexp(dir), oa, lsm, &oti, ll_i2mdexp(dir));
        obdo_free(oa);
        if (rc)
                CERROR("obd destroy objid "LPX64" error %d\n",
                       lsm->lsm_object_id, rc);
 out_free_memmd:
        obd_free_memmd(ll_i2dtexp(dir), &lsm);
 out:
        return rc;
}

static int ll_unlink_generic(struct inode *dir, struct qstr *name)
{
        struct ptlrpc_request *request = NULL;
        struct md_op_data *op_data;
        int rc;
        ENTRY;
        CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s,dir=%lu/%u(%p)\n",
               name->len, name->name, dir->i_ino, dir->i_generation, dir);

        op_data = ll_prep_md_op_data(NULL, dir, NULL, name->name, 
                                     name->len, 0, LUSTRE_OPC_ANY);
        if (op_data == NULL)
                RETURN(-ENOMEM);
        rc = md_unlink(ll_i2sbi(dir)->ll_md_exp, op_data, &request);
        ll_finish_md_op_data(op_data);
        
        if (rc)
                GOTO(out, rc);

        ll_update_times(request, REPLY_REC_OFF, dir);

        rc = ll_objects_destroy(request, dir);
 out:
        ptlrpc_req_finished(request);
        RETURN(rc);
}

static int ll_rename_generic(struct inode *src, struct qstr *src_name,
                             struct inode *tgt, struct qstr *tgt_name)
{
        struct ptlrpc_request *request = NULL;
        struct ll_sb_info *sbi = ll_i2sbi(src);
        struct md_op_data *op_data;
        int err;
        ENTRY;
        CDEBUG(D_VFSTRACE,"VFS Op:oldname=%.*s,src_dir=%lu/%u(%p),newname=%.*s,"
               "tgt_dir=%lu/%u(%p)\n", src_name->len, src_name->name,
               src->i_ino, src->i_generation, src, tgt_name->len,
               tgt_name->name, tgt->i_ino, tgt->i_generation, tgt);

        op_data = ll_prep_md_op_data(NULL, src, tgt, NULL, 0, 0, 
                                     LUSTRE_OPC_ANY);
        if (op_data == NULL)
                RETURN(-ENOMEM);
        err = md_rename(sbi->ll_md_exp, op_data,
                        src_name->name, src_name->len,
                        tgt_name->name, tgt_name->len, &request);
        ll_finish_md_op_data(op_data);
        if (!err) {
                ll_update_times(request, REPLY_REC_OFF, src);
                ll_update_times(request, REPLY_REC_OFF, tgt);
                err = ll_objects_destroy(request, src);
        }

        ptlrpc_req_finished(request);

        RETURN(err);
}

#ifdef LUSTRE_KERNEL_VERSION
static int ll_mknod_raw(struct nameidata *nd, int mode, dev_t rdev)
{
        return ll_mknod_generic(nd->dentry->d_inode, &nd->last, mode,rdev,NULL);
}
static int ll_rename_raw(struct nameidata *srcnd, struct nameidata *tgtnd)
{
        return ll_rename_generic(srcnd->dentry->d_inode, &srcnd->last,
                                 tgtnd->dentry->d_inode, &tgtnd->last);
}
static int ll_link_raw(struct nameidata *srcnd, struct nameidata *tgtnd)
{
        return ll_link_generic(srcnd->dentry->d_inode, tgtnd->dentry->d_inode,
                               &tgtnd->last);
}
static int ll_symlink_raw(struct nameidata *nd, const char *tgt)
{
        return -EOPNOTSUPP;
}
static int ll_rmdir_raw(struct nameidata *nd)
{
        return ll_rmdir_generic(nd->dentry->d_inode, nd->dentry, &nd->last);
}
static int ll_mkdir_raw(struct nameidata *nd, int mode)
{
        return ll_mkdir_generic(nd->dentry->d_inode, &nd->last, mode, NULL);
}
static int ll_unlink_raw(struct nameidata *nd)
{
        return ll_unlink_generic(nd->dentry->d_inode, &nd->last);
}
#endif

static int ll_mknod(struct inode *dir, struct dentry *dchild, int mode,
                    ll_dev_t rdev)
{
        return ll_mknod_generic(dir, &dchild->d_name, mode,
                                old_encode_dev(rdev), dchild);
}

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
static int ll_unlink(struct inode * dir, struct dentry *dentry)
{
        return ll_unlink_generic(dir, &dentry->d_name);
}
static int ll_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
        return ll_mkdir_generic(dir, &dentry->d_name, mode, dentry);
}
static int ll_rmdir(struct inode *dir, struct dentry *dentry)
{
        return ll_rmdir_generic(dir, NULL, &dentry->d_name);
}
static int ll_symlink(struct inode *dir, struct dentry *dentry,
                      const char *oldname)
{
        return ll_symlink_generic(dir, dentry, oldname);
}
static int ll_link(struct dentry *old_dentry, struct inode *dir,
                   struct dentry *new_dentry)
{
        return ll_link_generic(old_dentry->d_inode, dir, &new_dentry->d_name);
}
static int ll_rename(struct inode *old_dir, struct dentry *old_dentry,
                     struct inode *new_dir, struct dentry *new_dentry)
{
        return ll_rename_generic(old_dir, &old_dentry->d_name, new_dir,
                               &new_dentry->d_name);
}
#endif

struct inode_operations ll_dir_inode_operations = {
#ifdef LUSTRE_KERNEL_VERSION
        .link_raw           = ll_link_raw,
        .unlink_raw         = ll_unlink_raw,
        .symlink_raw        = ll_symlink_raw,
        .mkdir_raw          = ll_mkdir_raw,
        .rmdir_raw          = ll_rmdir_raw,
        .mknod_raw          = ll_mknod_raw,
        .rename_raw         = ll_rename_raw,
        .setattr            = ll_setattr,
        .setattr_raw        = ll_setattr_raw,
#endif
        .mknod              = ll_mknod,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
        .create_it          = ll_create_it,
        .lookup_it          = ll_lookup_it,
        .revalidate_it      = ll_inode_revalidate_it,
#else
        .lookup             = ll_lookup_nd,
        .create             = ll_create_nd,
        /* We need all these non-raw things for NFSD, to not patch it. */
        .unlink             = ll_unlink,
        .mkdir              = ll_mkdir,
        .rmdir              = ll_rmdir,
        .symlink            = ll_symlink,
        .link               = ll_link,
        .rename             = ll_rename,
        .setattr            = ll_setattr,
        .getattr            = ll_getattr,
#endif
        .permission         = ll_inode_permission,
        .setxattr           = ll_setxattr,
        .getxattr           = ll_getxattr,
        .listxattr          = ll_listxattr,
        .removexattr        = ll_removexattr,
};

struct inode_operations ll_special_inode_operations = {
#ifdef LUSTRE_KERNEL_VERSION
        .setattr_raw    = ll_setattr_raw,
#endif
        .setattr        = ll_setattr,
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0))
        .getattr        = ll_getattr,
#else   
        .revalidate_it  = ll_inode_revalidate_it,
#endif
        .permission     = ll_inode_permission,
        .setxattr       = ll_setxattr,
        .getxattr       = ll_getxattr,
        .listxattr      = ll_listxattr,
        .removexattr    = ll_removexattr,
};
