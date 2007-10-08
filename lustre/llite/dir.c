/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Directory code for lustre client.
 *
 *  Copyright (C) 2002--2007 Cluster File Systems, Inc.
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 *
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/version.h>
#include <linux/smp_lock.h>
#include <asm/uaccess.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
# include <linux/locks.h>   // for wait_on_buffer
#else
# include <linux/buffer_head.h>   // for wait_on_buffer
#endif

#define DEBUG_SUBSYSTEM S_LLITE

#include <obd_support.h>
#include <obd_class.h>
#include <lustre_lib.h>
#include <lustre/lustre_idl.h>
#include <lustre_lite.h>
#include <lustre_dlm.h>
#include "llite_internal.h"

/*
 * Directory entries are currently in the same format as ext2/ext3, but will
 * be changed in the future to accomodate FIDs
 */
#define LL_DIR_NAME_LEN (255)

static const int LL_DIR_PAD = 4;

struct ll_dir_entry {
        /* number of inode, referenced by this entry */
	__le32	lde_inode;
        /* total record length, multiple of LL_DIR_PAD */
	__le16	lde_rec_len;
        /* length of name */
	__u8	lde_name_len;
        /* file type: regular, directory, device, etc. */
	__u8	lde_file_type;
        /* name. NOT NUL-terminated */
	char	lde_name[LL_DIR_NAME_LEN];
};

static inline unsigned ll_dir_rec_len(unsigned name_len)
{
        return (name_len + 8 + LL_DIR_PAD - 1) & ~(LL_DIR_PAD - 1);
}


#ifdef HAVE_PG_FS_MISC
#define PageChecked(page)        test_bit(PG_fs_misc, &(page)->flags)
#define SetPageChecked(page)     set_bit(PG_fs_misc, &(page)->flags)
#endif

/* returns the page unlocked, but with a reference */
static int ll_dir_readpage(struct file *file, struct page *page)
{
        struct inode *inode = page->mapping->host;
        struct ll_fid mdc_fid;
        __u64 offset;
        struct ptlrpc_request *request;
        struct mds_body *body;
        int rc = 0;
        ENTRY;

        offset = (__u64)page->index << CFS_PAGE_SHIFT;
        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p) off "LPU64"\n",
               inode->i_ino, inode->i_generation, inode, offset);

        mdc_pack_fid(&mdc_fid, inode->i_ino, inode->i_generation, S_IFDIR);

        rc = mdc_readpage(ll_i2sbi(inode)->ll_mdc_exp, &mdc_fid,
                          offset, page, &request);
        if (!rc) {
                body = lustre_msg_buf(request->rq_repmsg, REPLY_REC_OFF,
                                      sizeof(*body));
                LASSERT(body != NULL); /* checked by mdc_readpage() */
                /* swabbed by mdc_readpage() */
                LASSERT_REPSWABBED(request, REPLY_REC_OFF);

                i_size_write(inode, body->size);
                SetPageUptodate(page);
        }
        ptlrpc_req_finished(request);

        unlock_page(page);
        EXIT;
        return rc;
}

struct address_space_operations ll_dir_aops = {
        .readpage  = ll_dir_readpage,
};

static inline unsigned ll_dir_page_mask(struct inode *inode)
{
        return ~(inode->i_sb->s_blocksize - 1);
}

/*
 * Check consistency of a single entry.
 */
static int ll_dir_check_entry(struct inode *dir, struct ll_dir_entry *ent,
                              unsigned offset, unsigned rec_len, pgoff_t index)
{
        const char *msg;

        /*
         * Consider adding more checks.
         */

        if (unlikely(rec_len < ll_dir_rec_len(1)))
                msg = "entry is too short";
        else if (unlikely(rec_len & 3))
                msg = "wrong alignment";
        else if (unlikely(rec_len < ll_dir_rec_len(ent->lde_name_len)))
                msg = "rec_len doesn't match name_len";
        else if (unlikely(((offset + rec_len - 1) ^ offset) &
                          ll_dir_page_mask(dir)))
                msg = "directory entry across blocks";
        else
                return 0;
        CERROR("%s: bad entry in directory %lu/%u: %s - "
               "offset=%lu+%u, inode=%lu, rec_len=%d,"
               " name_len=%d\n", ll_i2mdcexp(dir)->exp_obd->obd_name,
               dir->i_ino, dir->i_generation, msg,
               index << CFS_PAGE_SHIFT,
               offset, (unsigned long)le32_to_cpu(ent->lde_inode),
               rec_len, ent->lde_name_len);
        return -EIO;
}

static inline struct ll_dir_entry *ll_entry_at(void *base, unsigned offset)
{
        return (struct ll_dir_entry *)(base + offset);
}

static void ll_dir_check_page(struct inode *dir, struct page *page)
{
        int      err;
        unsigned size = dir->i_sb->s_blocksize;
        char    *addr = page_address(page);
        unsigned off;
        unsigned limit;
        unsigned reclen;

        struct ll_dir_entry *ent;

        err = 0;
        if ((i_size_read(dir) >> CFS_PAGE_SHIFT) == (__u64)page->index) {
                /*
                 * Last page.
                 */
                limit = i_size_read(dir) & ~CFS_PAGE_MASK;
                if (limit & (size - 1)) {
                        CERROR("%s: dir %lu/%u size %llu doesn't match %u\n",
                               ll_i2mdcexp(dir)->exp_obd->obd_name, dir->i_ino,
                               dir->i_generation, i_size_read(dir), size);
                        err++;
                } else {
                        /*
                         * Place dummy forwarding entries to streamline
                         * ll_readdir().
                         */
                        for (off = limit; off < CFS_PAGE_SIZE; off += size) {
                                ent = ll_entry_at(addr, off);
                                ent->lde_rec_len = cpu_to_le16(size);
                                ent->lde_name_len = 0;
                                ent->lde_inode = 0;
                        }
                }
        } else
                limit = CFS_PAGE_SIZE;

        for (off = 0;
             !err && off <= limit - ll_dir_rec_len(1); off += reclen) {
                ent    = ll_entry_at(addr, off);
                reclen = le16_to_cpu(ent->lde_rec_len);
                err    = ll_dir_check_entry(dir, ent, off, reclen, page->index);
        }

        if (!err && off != limit) {
                ent = ll_entry_at(addr, off);
                CERROR("%s: entry in directory %lu/%u spans the page boundary "
                       "offset="LPU64"+%u, inode=%lu\n",
                       ll_i2mdcexp(dir)->exp_obd->obd_name,
                       dir->i_ino, dir->i_generation,
                       (__u64)page->index << CFS_PAGE_SHIFT,
                       off, (unsigned long)le32_to_cpu(ent->lde_inode));
                err++;
        }
        if (err)
                SetPageError(page);
        SetPageChecked(page);
}

struct page *ll_get_dir_page(struct inode *dir, unsigned long n)
{
        struct ldlm_res_id res_id =
                { .name = { dir->i_ino, (__u64)dir->i_generation} };
        struct lustre_handle lockh;
        struct obd_device *obddev = class_exp2obd(ll_i2sbi(dir)->ll_mdc_exp);
        struct address_space *mapping = dir->i_mapping;
        struct page *page;
        ldlm_policy_data_t policy = {.l_inodebits = {MDS_INODELOCK_UPDATE} };
        int rc;

        rc = ldlm_lock_match(obddev->obd_namespace, LDLM_FL_BLOCK_GRANTED,
                             &res_id, LDLM_IBITS, &policy, LCK_CR, &lockh);
        if (!rc) {
                struct lookup_intent it = { .it_op = IT_READDIR };
                struct ldlm_enqueue_info einfo = { LDLM_IBITS, LCK_CR,
                       ll_mdc_blocking_ast, ldlm_completion_ast, NULL, dir };
                struct ptlrpc_request *request;
                struct mdc_op_data data;

                ll_prepare_mdc_op_data(&data, dir, NULL, NULL, 0, 0, NULL);

                rc = mdc_enqueue(ll_i2sbi(dir)->ll_mdc_exp, &einfo, &it,
                                 &data, &lockh, NULL, 0, 0);

                request = (struct ptlrpc_request *)it.d.lustre.it_data;
                if (request)
                        ptlrpc_req_finished(request);
                if (rc < 0) {
                        CERROR("lock enqueue: rc: %d\n", rc);
                        return ERR_PTR(rc);
                }
        }
        ldlm_lock_dump_handle(D_OTHER, &lockh);

        page = read_cache_page(mapping, n,
                               (filler_t*)mapping->a_ops->readpage, NULL);
        if (IS_ERR(page))
                GOTO(out_unlock, page);

        wait_on_page(page);
        (void)kmap(page);
        if (!PageUptodate(page))
                goto fail;
        if (!PageChecked(page))
                ll_dir_check_page(dir, page);
        if (PageError(page))
                goto fail;

out_unlock:
        ldlm_lock_decref(&lockh, LCK_CR);
        return page;

fail:
        kunmap(page);
        page_cache_release(page);
        page = ERR_PTR(-EIO);
        goto out_unlock;
}

/*
 * p is at least 6 bytes before the end of page
 */
static inline struct ll_dir_entry *ll_dir_next_entry(struct ll_dir_entry *p)
{
        return ll_entry_at(p, le16_to_cpu(p->lde_rec_len));
}

static inline unsigned ll_dir_validate_entry(char *base, unsigned offset,
                                             unsigned mask)
{
        struct ll_dir_entry *de = ll_entry_at(base, offset);
        struct ll_dir_entry *p  = ll_entry_at(base, offset & mask);
        while (p < de && p->lde_rec_len > 0)
                p = ll_dir_next_entry(p);
        return (char *)p - base;
}

/*
 * File type constants. The same as in ext2 for compatibility.
 */

enum {
        LL_DIR_FT_UNKNOWN,
        LL_DIR_FT_REG_FILE,
        LL_DIR_FT_DIR,
        LL_DIR_FT_CHRDEV,
        LL_DIR_FT_BLKDEV,
        LL_DIR_FT_FIFO,
        LL_DIR_FT_SOCK,
        LL_DIR_FT_SYMLINK,
        LL_DIR_FT_MAX
};

static unsigned char ll_dir_filetype_table[LL_DIR_FT_MAX] = {
        [LL_DIR_FT_UNKNOWN]  = DT_UNKNOWN,
        [LL_DIR_FT_REG_FILE] = DT_REG,
        [LL_DIR_FT_DIR]      = DT_DIR,
        [LL_DIR_FT_CHRDEV]   = DT_CHR,
        [LL_DIR_FT_BLKDEV]   = DT_BLK,
        [LL_DIR_FT_FIFO]     = DT_FIFO,
        [LL_DIR_FT_SOCK]     = DT_SOCK,
        [LL_DIR_FT_SYMLINK]  = DT_LNK,
};

/*
 * Process one page. Returns:
 *
 *     -ve: filldir commands readdir to stop.
 *     +ve: number of entries submitted to filldir.
 *       0: no live entries on this page.
 */

int ll_readdir_page(char *addr, __u64 base, unsigned *offset,
                    filldir_t filldir, void *cookie)
{
        struct ll_dir_entry *de;
        char *end;
        int nr;

        de = ll_entry_at(addr, *offset);
        end = addr + CFS_PAGE_SIZE - ll_dir_rec_len(1);
        for (nr = 0 ;(char*)de <= end; de = ll_dir_next_entry(de)) {
                if (de->lde_inode != 0) {
                        nr++;
                        *offset = (char *)de - addr;
                        if (filldir(cookie, de->lde_name, de->lde_name_len,
                                    base | *offset, le32_to_cpu(de->lde_inode),
                                    ll_dir_filetype_table[de->lde_file_type &
                                                          (LL_DIR_FT_MAX - 1)]))
                                return -1;
                }
        }
        return nr;
}

int ll_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
        struct inode *inode = filp->f_dentry->d_inode;
        loff_t pos          = filp->f_pos;
        unsigned offset     = pos & ~CFS_PAGE_MASK;
        pgoff_t idx         = pos >> CFS_PAGE_SHIFT;
        pgoff_t npages      = dir_pages(inode);
        unsigned chunk_mask = ll_dir_page_mask(inode);
        int need_revalidate = (filp->f_version != inode->i_version);
        int rc              = 0;
        int done; /* when this becomes negative --- stop iterating */

        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p) pos %llu/%llu\n",
               inode->i_ino, inode->i_generation, inode,
               pos, i_size_read(inode));

        /*
         * Checking ->i_size without the lock. Should be harmless, as server
         * re-checks.
         */
        if (pos > i_size_read(inode) - ll_dir_rec_len(1))
                RETURN(0);

        for (done = 0; idx < npages; idx++, offset = 0) {
                /*
                 * We can assume that all blocks on this page are filled with
                 * entries, because ll_dir_check_page() placed special dummy
                 * entries for us.
                 */

                char *kaddr;
                struct page *page;

                CDEBUG(D_EXT2,"read %lu of dir %lu/%u page %lu/%lu "
                       "size %llu\n",
                       CFS_PAGE_SIZE, inode->i_ino, inode->i_generation,
                       idx, npages, i_size_read(inode));
                page = ll_get_dir_page(inode, idx);

                /* size might have been updated by mdc_readpage */
                npages = dir_pages(inode);

                if (IS_ERR(page)) {
                        rc = PTR_ERR(page);
                        CERROR("error reading dir %lu/%u page %lu: rc %d\n",
                               inode->i_ino, inode->i_generation, idx, rc);
                        continue;
                }

                kaddr = page_address(page);
                if (need_revalidate) {
                        /*
                         * File offset was changed by lseek() and possibly
                         * points in the middle of an entry. Re-scan from the
                         * beginning of the chunk.
                         */
                        offset = ll_dir_validate_entry(kaddr, offset,
                                                       chunk_mask);
                        need_revalidate = 0;
                }
                done = ll_readdir_page(kaddr, idx << CFS_PAGE_SHIFT,
                                       &offset, filldir, dirent);
                kunmap(page);
                page_cache_release(page);
                if (done > 0)
                        /*
                         * Some entries were sent to the user space, return
                         * success.
                         */
                        rc = 0;
                else if (done < 0)
                        /*
                         * filldir is satisfied.
                         */
                        break;
        }

        filp->f_pos = (idx << CFS_PAGE_SHIFT) | offset;
        filp->f_version = inode->i_version;
        touch_atime(filp->f_vfsmnt, filp->f_dentry);

        RETURN(rc);
}

#define QCTL_COPY(out, in)              \
do {                                    \
        Q_COPY(out, in, qc_cmd);        \
        Q_COPY(out, in, qc_type);       \
        Q_COPY(out, in, qc_id);         \
        Q_COPY(out, in, qc_stat);       \
        Q_COPY(out, in, qc_dqinfo);     \
        Q_COPY(out, in, qc_dqblk);      \
} while (0)

int ll_send_mgc_param(struct obd_export *mgc, char *string)
{
        struct mgs_send_param *msp;
        int rc = 0;

        OBD_ALLOC_PTR(msp);
        if (!msp)
                return -ENOMEM;

        strncpy(msp->mgs_param, string, MGS_PARAM_MAXLEN);
        rc = obd_set_info_async(mgc, strlen(KEY_SET_INFO), KEY_SET_INFO,
                                sizeof(struct mgs_send_param), msp, NULL);
        if (rc)
                CERROR("Failed to set parameter: %d\n", rc);

        OBD_FREE_PTR(msp);
        return rc;
}

char *ll_get_fsname(struct inode *inode)
{
        struct lustre_sb_info *lsi = s2lsi(inode->i_sb);
        char *ptr, *fsname;
        int len;

        OBD_ALLOC(fsname, MGS_PARAM_MAXLEN);
        len = strlen(lsi->lsi_lmd->lmd_profile);
        ptr = strrchr(lsi->lsi_lmd->lmd_profile, '-');
        if (ptr && (strcmp(ptr, "-client") == 0))
                len -= 7;
        strncpy(fsname, lsi->lsi_lmd->lmd_profile, len);
        fsname[len] = '\0';

        return fsname;
}

int ll_dir_setstripe(struct inode *inode, struct lov_user_md *lump,
                     int set_default)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct mdc_op_data data;
        struct ptlrpc_request *req = NULL;
        struct lustre_sb_info *lsi = s2lsi(inode->i_sb);
        struct obd_device *mgc = lsi->lsi_mgc;
        char *fsname = NULL, *param = NULL;

        struct iattr attr = { 0 };
        int rc = 0;

        /*
         * This is coming from userspace, so should be in
         * local endian.  But the MDS would like it in little
         * endian, so we swab it before we send it.
         */
        if (lump->lmm_magic != LOV_USER_MAGIC)
                RETURN(-EINVAL);

        if (lump->lmm_magic != cpu_to_le32(LOV_USER_MAGIC))
                lustre_swab_lov_user_md(lump);

        ll_prepare_mdc_op_data(&data, inode, NULL, NULL, 0, 0, NULL);

        /* swabbing is done in lov_setstripe() on server side */
        rc = mdc_setattr(sbi->ll_mdc_exp, &data,
                         &attr, lump, sizeof(*lump), NULL, 0, &req);
        if (rc) {
                ptlrpc_req_finished(req);
                if (rc != -EPERM && rc != -EACCES)
                        CERROR("mdc_setattr fails: rc = %d\n", rc);
                return rc;
        }
        ptlrpc_req_finished(req);

        if (set_default && mgc->u.cli.cl_mgc_mgsexp) {
                OBD_ALLOC(param, MGS_PARAM_MAXLEN);

                /* Get fsname and assume devname to be -MDT0000. */
                fsname = ll_get_fsname(inode);
                /* Set root stripesize */
                sprintf(param, "%s-MDT0000.lov.stripesize=%u", fsname,
                        lump->lmm_stripe_size);
                rc = ll_send_mgc_param(mgc->u.cli.cl_mgc_mgsexp, param);
                if (rc)
                        goto end;

                /* Set root stripecount */
                sprintf(param, "%s-MDT0000.lov.stripecount=%u", fsname,
                        lump->lmm_stripe_count);
                rc = ll_send_mgc_param(mgc->u.cli.cl_mgc_mgsexp, param);
                if (rc)
                        goto end;

                /* Set root stripeoffset */
                sprintf(param, "%s-MDT0000.lov.stripeoffset=%u", fsname,
                        lump->lmm_stripe_offset);
                rc = ll_send_mgc_param(mgc->u.cli.cl_mgc_mgsexp, param);
                if (rc)
                        goto end;
end:
                if (fsname)
                        OBD_FREE(fsname, MGS_PARAM_MAXLEN);
                if (param)
                        OBD_FREE(param, MGS_PARAM_MAXLEN);
        }
        return rc;
}

int ll_dir_getstripe(struct inode *inode, struct lov_mds_md **lmmp,
                     int *lmm_size, struct ptlrpc_request **request)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ll_fid     fid;
        struct mds_body   *body;
        struct lov_mds_md *lmm = NULL;
        struct ptlrpc_request *req = NULL;
        int rc, lmmsize;

        ll_inode2fid(&fid, inode);

        rc = ll_get_max_mdsize(sbi, &lmmsize);
        if (rc)
                RETURN(rc);

        rc = mdc_getattr(sbi->ll_mdc_exp, &fid,
                        OBD_MD_FLEASIZE|OBD_MD_FLDIREA,
                        lmmsize, &req);
        if (rc < 0) {
                CDEBUG(D_INFO, "mdc_getattr failed on inode "
                       "%lu/%u: rc %d\n", inode->i_ino,
                       inode->i_generation, rc);
                GOTO(out, rc);
        }
        body = lustre_msg_buf(req->rq_repmsg, REPLY_REC_OFF,
                        sizeof(*body));
        LASSERT(body != NULL); /* checked by mdc_getattr_name */
        /* swabbed by mdc_getattr_name */
        LASSERT_REPSWABBED(req, REPLY_REC_OFF);

        lmmsize = body->eadatasize;

        if (!(body->valid & (OBD_MD_FLEASIZE | OBD_MD_FLDIREA)) ||
            lmmsize == 0) {
                GOTO(out, rc = -ENODATA);
        }

        lmm = lustre_msg_buf(req->rq_repmsg, REPLY_REC_OFF + 1, lmmsize);
        LASSERT(lmm != NULL);
        LASSERT_REPSWABBED(req, REPLY_REC_OFF + 1);

        /*
         * This is coming from the MDS, so is probably in
         * little endian.  We convert it to host endian before
         * passing it to userspace.
         */
        if (lmm->lmm_magic == __swab32(LOV_MAGIC)) {
                lustre_swab_lov_user_md((struct lov_user_md *)lmm);
                lustre_swab_lov_user_md_objects((struct lov_user_md *)lmm);
        }
out:
        *lmmp = lmm;
        *lmm_size = lmmsize;
        *request = req;
        return rc;
}

static int ll_dir_ioctl(struct inode *inode, struct file *file,
                        unsigned int cmd, unsigned long arg)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct obd_ioctl_data *data;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode=%lu/%u(%p), cmd=%#x\n",
               inode->i_ino, inode->i_generation, inode, cmd);

        /* asm-ppc{,64} declares TCGETS, et. al. as type 't' not 'T' */
        if (_IOC_TYPE(cmd) == 'T' || _IOC_TYPE(cmd) == 't') /* tty ioctls */
                return -ENOTTY;

        ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_IOCTL, 1);
        switch(cmd) {
        case EXT3_IOC_GETFLAGS:
        case EXT3_IOC_SETFLAGS:
                RETURN(ll_iocontrol(inode, file, cmd, arg));
        case EXT3_IOC_GETVERSION_OLD:
        case EXT3_IOC_GETVERSION:
                RETURN(put_user(inode->i_generation, (int *)arg));
        /* We need to special case any other ioctls we want to handle,
         * to send them to the MDS/OST as appropriate and to properly
         * network encode the arg field.
        case EXT3_IOC_SETVERSION_OLD:
        case EXT3_IOC_SETVERSION:
        */
        case IOC_MDC_LOOKUP: {
                struct ptlrpc_request *request = NULL;
                struct ll_fid fid;
                char *buf = NULL;
                char *filename;
                int namelen, rc, len = 0;

                rc = obd_ioctl_getdata(&buf, &len, (void *)arg);
                if (rc)
                        RETURN(rc);
                data = (void *)buf;

                filename = data->ioc_inlbuf1;
                namelen = data->ioc_inllen1;

                if (namelen < 1) {
                        CDEBUG(D_INFO, "IOC_MDC_LOOKUP missing filename\n");
                        GOTO(out, rc = -EINVAL);
                }

                ll_inode2fid(&fid, inode);
                rc = mdc_getattr_name(sbi->ll_mdc_exp, &fid, filename, namelen,
                                      OBD_MD_FLID, 0, &request);
                if (rc < 0) {
                        CDEBUG(D_INFO, "mdc_getattr_name: %d\n", rc);
                        GOTO(out, rc);
                }

                ptlrpc_req_finished(request);

                EXIT;
        out:
                obd_ioctl_freedata(buf, len);
                return rc;
        }
        case LL_IOC_LOV_SETSTRIPE: {
                struct lov_user_md lum, *lump = (struct lov_user_md *)arg;
                int rc = 0;
                int set_default = 0;

                LASSERT(sizeof(lum) == sizeof(*lump));
                LASSERT(sizeof(lum.lmm_objects[0]) ==
                        sizeof(lump->lmm_objects[0]));
                rc = copy_from_user(&lum, lump, sizeof(lum));
                if (rc)
                        return(-EFAULT);

                if (inode->i_sb->s_root == file->f_dentry)
                        set_default = 1;

                rc = ll_dir_setstripe(inode, &lum, set_default);

                return rc;
        }
        case LL_IOC_OBD_STATFS:
                RETURN(ll_obd_statfs(inode, (void *)arg));
        case LL_IOC_LOV_GETSTRIPE:
        case LL_IOC_MDC_GETINFO:
        case IOC_MDC_GETFILEINFO:
        case IOC_MDC_GETFILESTRIPE: {
                struct ptlrpc_request *request = NULL;
                struct mds_body *body;
                struct lov_user_md *lump;
                struct lov_mds_md *lmm = NULL;
                char *filename = NULL;
                int rc, lmmsize;

                if (cmd == IOC_MDC_GETFILEINFO ||
                    cmd == IOC_MDC_GETFILESTRIPE) {
                        filename = getname((const char *)arg);
                        if (IS_ERR(filename))
                                RETURN(PTR_ERR(filename));

                        rc = ll_lov_getstripe_ea_info(inode, filename, &lmm,
                                                      &lmmsize, &request);
                } else {
                        rc = ll_dir_getstripe(inode, &lmm, &lmmsize, &request);
                }

                if (request) {
                        body = lustre_msg_buf(request->rq_repmsg, REPLY_REC_OFF,
                                              sizeof(*body));
                        LASSERT(body != NULL); /* checked by mdc_getattr_name */
                        /* swabbed by mdc_getattr_name */
                        LASSERT_REPSWABBED(request, REPLY_REC_OFF);
                } else {
                        GOTO(out_req, rc);
                }

                if (rc < 0) {
                        if (rc == -ENODATA && (cmd == IOC_MDC_GETFILEINFO ||
                                               cmd == LL_IOC_MDC_GETINFO))
                                GOTO(skip_lmm, rc = 0);
                        else
                                GOTO(out_req, rc);
                }

                if (cmd == IOC_MDC_GETFILESTRIPE ||
                    cmd == LL_IOC_LOV_GETSTRIPE) {
                        lump = (struct lov_user_md *)arg;
                } else {
                        struct lov_user_mds_data *lmdp;
                        lmdp = (struct lov_user_mds_data *)arg;
                        lump = &lmdp->lmd_lmm;
                }
                rc = copy_to_user(lump, lmm, lmmsize);
                if (rc)
                        GOTO(out_lmm, rc = -EFAULT);
        skip_lmm:
                if (cmd == IOC_MDC_GETFILEINFO || cmd == LL_IOC_MDC_GETINFO) {
                        struct lov_user_mds_data *lmdp;
                        lstat_t st = { 0 };

                        st.st_dev     = inode->i_sb->s_dev;
                        st.st_mode    = body->mode;
                        st.st_nlink   = body->nlink;
                        st.st_uid     = body->uid;
                        st.st_gid     = body->gid;
                        st.st_rdev    = body->rdev;
                        st.st_size    = body->size;
                        st.st_blksize = CFS_PAGE_SIZE;
                        st.st_blocks  = body->blocks;
                        st.st_atime   = body->atime;
                        st.st_mtime   = body->mtime;
                        st.st_ctime   = body->ctime;
                        st.st_ino     = body->ino;

                        lmdp = (struct lov_user_mds_data *)arg;
                        rc = copy_to_user(&lmdp->lmd_st, &st, sizeof(st));
                        if (rc)
                                GOTO(out_lmm, rc = -EFAULT);
                }

                EXIT;
        out_lmm:
                if (lmm && lmm->lmm_magic == LOV_MAGIC_JOIN)
                        OBD_FREE(lmm, lmmsize);
        out_req:
                ptlrpc_req_finished(request);
                if (filename)
                        putname(filename);
                return rc;
        }
        case IOC_LOV_GETINFO: {
                struct lov_user_mds_data *lumd;
                struct lov_stripe_md *lsm;
                struct lov_user_md *lum;
                struct lov_mds_md *lmm;
                int lmmsize;
                lstat_t st;
                int rc;

                lumd = (struct lov_user_mds_data *)arg;
                lum = &lumd->lmd_lmm;

                rc = ll_get_max_mdsize(sbi, &lmmsize);
                if (rc)
                        RETURN(rc);

                OBD_ALLOC(lmm, lmmsize);
                rc = copy_from_user(lmm, lum, lmmsize);
                if (rc)
                        GOTO(free_lmm, rc = -EFAULT);

                rc = obd_unpackmd(sbi->ll_osc_exp, &lsm, lmm, lmmsize);
                if (rc < 0)
                        GOTO(free_lmm, rc = -ENOMEM);

                rc = obd_checkmd(sbi->ll_osc_exp, sbi->ll_mdc_exp, lsm);
                if (rc)
                        GOTO(free_lsm, rc);

                /* Perform glimpse_size operation. */
                memset(&st, 0, sizeof(st));

                rc = ll_glimpse_ioctl(sbi, lsm, &st);
                if (rc)
                        GOTO(free_lsm, rc);

                rc = copy_to_user(&lumd->lmd_st, &st, sizeof(st));
                if (rc)
                        GOTO(free_lsm, rc = -EFAULT);

                EXIT;
        free_lsm:
                obd_free_memmd(sbi->ll_osc_exp, &lsm);
        free_lmm:
                OBD_FREE(lmm, lmmsize);
                return rc;
        }
        case OBD_IOC_LLOG_CATINFO: {
                struct ptlrpc_request *req = NULL;
                char *buf = NULL;
                int rc, len = 0;
                char *bufs[3] = { NULL }, *str;
                int lens[3] = { sizeof(struct ptlrpc_body) };
                int size[2] = { sizeof(struct ptlrpc_body) };

                rc = obd_ioctl_getdata(&buf, &len, (void *)arg);
                if (rc)
                        RETURN(rc);
                data = (void *)buf;

                if (!data->ioc_inlbuf1) {
                        obd_ioctl_freedata(buf, len);
                        RETURN(-EINVAL);
                }

                lens[REQ_REC_OFF] = data->ioc_inllen1;
                bufs[REQ_REC_OFF] = data->ioc_inlbuf1;
                if (data->ioc_inllen2) {
                        lens[REQ_REC_OFF + 1] = data->ioc_inllen2;
                        bufs[REQ_REC_OFF + 1] = data->ioc_inlbuf2;
                } else {
                        lens[REQ_REC_OFF + 1] = 0;
                        bufs[REQ_REC_OFF + 1] = NULL;
                }

                req = ptlrpc_prep_req(sbi2mdc(sbi)->cl_import,
                                      LUSTRE_LOG_VERSION, LLOG_CATINFO, 3, lens,
                                      bufs);
                if (!req)
                        GOTO(out_catinfo, rc = -ENOMEM);

                size[REPLY_REC_OFF] = data->ioc_plen1;
                ptlrpc_req_set_repsize(req, 2, size);

                rc = ptlrpc_queue_wait(req);
                str = lustre_msg_string(req->rq_repmsg, REPLY_REC_OFF,
                                        data->ioc_plen1);
                if (!rc)
                        rc = copy_to_user(data->ioc_pbuf1, str,data->ioc_plen1);
                ptlrpc_req_finished(req);
        out_catinfo:
                obd_ioctl_freedata(buf, len);
                RETURN(rc);
        }
        case OBD_IOC_QUOTACHECK: {
                struct obd_quotactl *oqctl;
                int rc, error = 0;

                if (!capable(CAP_SYS_ADMIN))
                        RETURN(-EPERM);

                OBD_ALLOC_PTR(oqctl);
                if (!oqctl)
                        RETURN(-ENOMEM);
                oqctl->qc_type = arg;
                rc = obd_quotacheck(sbi->ll_mdc_exp, oqctl);
                if (rc < 0) {
                        CDEBUG(D_INFO, "mdc_quotacheck failed: rc %d\n", rc);
                        error = rc;
                }

                rc = obd_quotacheck(sbi->ll_osc_exp, oqctl);
                if (rc < 0)
                        CDEBUG(D_INFO, "osc_quotacheck failed: rc %d\n", rc);

                OBD_FREE_PTR(oqctl);
                return error ?: rc;
        }
        case OBD_IOC_POLL_QUOTACHECK: {
                struct if_quotacheck *check;
                int rc;

                if (!capable(CAP_SYS_ADMIN))
                        RETURN(-EPERM);

                OBD_ALLOC_PTR(check);
                if (!check)
                        RETURN(-ENOMEM);

                rc = obd_iocontrol(cmd, sbi->ll_mdc_exp, 0, (void *)check,
                                   NULL);
                if (rc) {
                        CDEBUG(D_QUOTA, "mdc ioctl %d failed: %d\n", cmd, rc);
                        if (copy_to_user((void *)arg, check, sizeof(*check)))
                                rc = -EFAULT;
                        GOTO(out_poll, rc);
                }

                rc = obd_iocontrol(cmd, sbi->ll_osc_exp, 0, (void *)check,
                                   NULL);
                if (rc) {
                        CDEBUG(D_QUOTA, "osc ioctl %d failed: %d\n", cmd, rc);
                        if (copy_to_user((void *)arg, check, sizeof(*check)))
                                rc = -EFAULT;
                        GOTO(out_poll, rc);
                }
        out_poll:
                OBD_FREE_PTR(check);
                RETURN(rc);
        }
#ifdef HAVE_QUOTA_SUPPORT
        case OBD_IOC_QUOTACTL: {
                struct if_quotactl *qctl;
                struct obd_quotactl *oqctl;

                int cmd, type, id, rc = 0;

                OBD_ALLOC_PTR(qctl);
                if (!qctl)
                        RETURN(-ENOMEM);

                OBD_ALLOC_PTR(oqctl);
                if (!oqctl) {
                        OBD_FREE_PTR(qctl);
                        RETURN(-ENOMEM);
                }
                if (copy_from_user(qctl, (void *)arg, sizeof(*qctl)))
                        GOTO(out_quotactl, rc = -EFAULT);

                cmd = qctl->qc_cmd;
                type = qctl->qc_type;
                id = qctl->qc_id;
                switch (cmd) {
                case Q_QUOTAON:
                case Q_QUOTAOFF:
                case Q_SETQUOTA:
                case Q_SETINFO:
                        if (!capable(CAP_SYS_ADMIN))
                                GOTO(out_quotactl, rc = -EPERM);
                        break;
                case Q_GETQUOTA:
                        if (((type == USRQUOTA && current->euid != id) ||
                             (type == GRPQUOTA && !in_egroup_p(id))) &&
                            !capable(CAP_SYS_ADMIN))
                                GOTO(out_quotactl, rc = -EPERM);

                        /* XXX: dqb_valid is borrowed as a flag to mark that
                         *      only mds quota is wanted */
                        if (qctl->qc_dqblk.dqb_valid)
                                qctl->obd_uuid = sbi->ll_mdc_exp->exp_obd->
                                                        u.cli.cl_target_uuid;
                        break;
                case Q_GETINFO:
                        break;
                default:
                        CERROR("unsupported quotactl op: %#x\n", cmd);
                        GOTO(out_quotactl, -ENOTTY);
                }

                QCTL_COPY(oqctl, qctl);

                if (qctl->obd_uuid.uuid[0]) {
                        struct obd_device *obd;
                        struct obd_uuid *uuid = &qctl->obd_uuid;

                        obd = class_find_client_notype(uuid,
                                         &sbi->ll_osc_exp->exp_obd->obd_uuid);
                        if (!obd)
                                GOTO(out_quotactl, rc = -ENOENT);

                        if (cmd == Q_GETINFO)
                                oqctl->qc_cmd = Q_GETOINFO;
                        else if (cmd == Q_GETQUOTA)
                                oqctl->qc_cmd = Q_GETOQUOTA;
                        else
                                GOTO(out_quotactl, rc = -EINVAL);

                        if (sbi->ll_mdc_exp->exp_obd == obd) {
                                rc = obd_quotactl(sbi->ll_mdc_exp, oqctl);
                        } else {
                                int i;
                                struct obd_export *exp;
                                struct lov_obd *lov = &sbi->ll_osc_exp->
                                                            exp_obd->u.lov;

                                for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                                        if (!lov->lov_tgts[i] ||
                                            !lov->lov_tgts[i]->ltd_active)
                                                continue;
                                        exp = lov->lov_tgts[i]->ltd_exp;
                                        if (exp->exp_obd == obd) {
                                                rc = obd_quotactl(exp, oqctl);
                                                break;
                                        }
                                }
                        }

                        oqctl->qc_cmd = cmd;
                        QCTL_COPY(qctl, oqctl);

                        if (copy_to_user((void *)arg, qctl, sizeof(*qctl)))
                                rc = -EFAULT;

                        GOTO(out_quotactl, rc);
                }

                rc = obd_quotactl(sbi->ll_mdc_exp, oqctl);
                if (rc && rc != -EBUSY && cmd == Q_QUOTAON) {
                        oqctl->qc_cmd = Q_QUOTAOFF;
                        obd_quotactl(sbi->ll_mdc_exp, oqctl);
                }

                QCTL_COPY(qctl, oqctl);

                if (copy_to_user((void *)arg, qctl, sizeof(*qctl)))
                        rc = -EFAULT;
        out_quotactl:
                OBD_FREE_PTR(qctl);
                OBD_FREE_PTR(oqctl);
                RETURN(rc);
        }
#endif /* HAVE_QUOTA_SUPPORT */
        case OBD_IOC_GETNAME_OLD:
        case OBD_IOC_GETNAME: {
                struct obd_device *obd = class_exp2obd(sbi->ll_osc_exp);
                if (!obd)
                        RETURN(-EFAULT);
                if (copy_to_user((void *)arg, obd->obd_name,
                                strlen(obd->obd_name) + 1))
                        RETURN (-EFAULT);
                RETURN(0);
        }
        default:
                RETURN(obd_iocontrol(cmd, sbi->ll_osc_exp,0,NULL,(void *)arg));
        }
}

struct file_operations ll_dir_operations = {
        .open     = ll_file_open,
        .release  = ll_file_release,
        .read     = generic_read_dir,
        .readdir  = ll_readdir,
        .ioctl    = ll_dir_ioctl
};

