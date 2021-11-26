/**
 * super.c - in this file we implement functions to handle the superblock.
 *
 * This file is derived from simplefs.
 * Original Author: 
 *   Jim Huang <jserv.tw@gmail.com>
 *
 * One change is we don't use inode 0 for the root inode, instead we use inode 2, which is the same choice as in the ext2 file system.
 * Author:
 *   Jidong Xiao <jidongxiao@boisestate.edu>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h> /* so we can use sb_bread() */
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "booga.h"

static struct kmem_cache *boogafs_inode_cache;

int boogafs_init_inodecache(void)
{
    boogafs_inode_cache = kmem_cache_create(
        "boogafs_cache", sizeof(struct boogafs_inode_info), 0, 0, NULL);
    if (!boogafs_inode_cache)
        return -ENOMEM;
    return 0;
}

void boogafs_destroy_inodecache(void)
{
    kmem_cache_destroy(boogafs_inode_cache);
}

static struct inode *boogafs_alloc_inode(struct super_block *sb)
{
    struct boogafs_inode_info *ci =
        kmem_cache_alloc(boogafs_inode_cache, GFP_KERNEL);
    if (!ci)
        return NULL;

    pr_info("this function is called during mount\n"); /* either: fill_super()-> boogafs_iget() -> iget_locked() -> alloc_inode(sb) -> sb->s_op->alloc_inode(sb) 
                                                          or: fill_super() -> boogfs_iget() -> new_inode() -> new_inode_pseudo() -> alloc_inode(sb) -> sb->s_op->alloc_inode(sb)
                                                        */
    inode_init_once(&ci->vfs_inode);
    return &ci->vfs_inode;
}

static void boogafs_destroy_inode(struct inode *inode)
{
    struct boogafs_inode_info *ci = BOOGAFS_INODE(inode);
    kmem_cache_free(boogafs_inode_cache, ci);
}

static int boogafs_write_inode(struct inode *inode,
                                struct writeback_control *wbc)
{
    struct boogafs_inode *disk_inode;
    struct boogafs_inode_info *ci = BOOGAFS_INODE(inode);
    struct super_block *sb = inode->i_sb;
    struct boogafs_sb_info *sbi = BOOGAFS_SB(sb);
    struct buffer_head *bh;
    uint32_t ino = inode->i_ino;
    uint32_t inode_block = (ino / BOOGAFS_INODES_PER_BLOCK) + 1;
    uint32_t inode_shift = ino % BOOGAFS_INODES_PER_BLOCK;

    if (ino >= sbi->nr_inodes)
        return 0;

    bh = sb_bread(sb, inode_block);
    if (!bh)
        return -EIO;

    disk_inode = (struct boogafs_inode *) bh->b_data;
    disk_inode += inode_shift;

    /* update the mode using what the generic inode has */
    disk_inode->i_mode = inode->i_mode;
    disk_inode->i_uid = i_uid_read(inode);
    disk_inode->i_gid = i_gid_read(inode);
    disk_inode->i_size = inode->i_size;
    disk_inode->i_ctime = inode->i_ctime.tv_sec;
    disk_inode->i_atime = inode->i_atime.tv_sec;
    disk_inode->i_mtime = inode->i_mtime.tv_sec;
    disk_inode->i_nlink = inode->i_nlink;
    disk_inode->data_block = ci->data_block;

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    return 0;
}

static void boogafs_put_super(struct super_block *sb)
{
    struct boogafs_sb_info *sbi = BOOGAFS_SB(sb);
    if (sbi) {
        kfree(sbi->inode_bitmap);
        kfree(sbi->data_bitmap);
        kfree(sbi);
    }
}

static int boogafs_sync_fs(struct super_block *sb, int wait)
{
    struct boogafs_sb_info *sbi = BOOGAFS_SB(sb);
    struct boogafs_sb_info *disk_sb;
    int i;

    /* Flush superblock */
    struct buffer_head *bh = sb_bread(sb, 0);
    if (!bh)
        return -EIO;

    disk_sb = (struct boogafs_sb_info *) bh->b_data;

    disk_sb->nr_blocks = sbi->nr_blocks;
    disk_sb->nr_inodes = sbi->nr_inodes;
    disk_sb->nr_itable_blocks = sbi->nr_itable_blocks;
    disk_sb->nr_ibitmap_blocks = sbi->nr_ibitmap_blocks;
    disk_sb->nr_dbitmap_blocks = sbi->nr_dbitmap_blocks;
    disk_sb->nr_free_inodes = sbi->nr_free_inodes;
    disk_sb->nr_free_blocks = sbi->nr_free_blocks;

    mark_buffer_dirty(bh);
    if (wait)
        sync_dirty_buffer(bh);
    brelse(bh);

    /* Flush free inodes bitmask */
    for (i = 0; i < sbi->nr_ibitmap_blocks; i++) {
        int idx = sbi->nr_itable_blocks + i + 1;

        bh = sb_bread(sb, idx);
        if (!bh)
            return -EIO;

        memcpy(bh->b_data, (void *) sbi->inode_bitmap + i * BOOGAFS_BLOCK_SIZE,
               BOOGAFS_BLOCK_SIZE);

        mark_buffer_dirty(bh);
        if (wait)
            sync_dirty_buffer(bh);
        brelse(bh);
    }

    /* Flush free blocks bitmask */
    for (i = 0; i < sbi->nr_dbitmap_blocks; i++) {
        int idx = sbi->nr_itable_blocks + sbi->nr_ibitmap_blocks + i + 1;

        bh = sb_bread(sb, idx);
        if (!bh)
            return -EIO;

        memcpy(bh->b_data, (void *) sbi->data_bitmap + i * BOOGAFS_BLOCK_SIZE,
               BOOGAFS_BLOCK_SIZE);

        mark_buffer_dirty(bh);
        if (wait)
            sync_dirty_buffer(bh);
        brelse(bh);
    }

    return 0;
}

static int boogafs_statfs(struct dentry *dentry, struct kstatfs *stat)
{
    struct super_block *sb = dentry->d_sb;
    struct boogafs_sb_info *sbi = BOOGAFS_SB(sb);

    stat->f_type = BOOGAFS_MAGIC;
    stat->f_bsize = BOOGAFS_BLOCK_SIZE;
    stat->f_blocks = sbi->nr_blocks;
    stat->f_bfree = sbi->nr_free_blocks;
    stat->f_bavail = sbi->nr_free_blocks;
    stat->f_files = sbi->nr_inodes - sbi->nr_free_inodes;
    stat->f_ffree = sbi->nr_free_inodes;
    stat->f_namelen = BOOGAFS_FILENAME_LEN;

    return 0;
}

static struct super_operations boogafs_super_ops = {
    .put_super = boogafs_put_super,
    .alloc_inode = boogafs_alloc_inode,
    .destroy_inode = boogafs_destroy_inode,
    .write_inode = boogafs_write_inode,
    .sync_fs = boogafs_sync_fs,
    .statfs = boogafs_statfs,
};

/* Fill the struct superblock from partition superblock */
int boogafs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct buffer_head *bh = NULL;
    struct boogafs_sb_info *csb = NULL;
    struct boogafs_sb_info *sbi = NULL;
    struct inode *root_inode = NULL;
    int ret = 0, i;

    pr_info("fill super block\n");
    /* Init sb */
    sb->s_magic = BOOGAFS_MAGIC;
    sb_set_blocksize(sb, BOOGAFS_BLOCK_SIZE); /* set sb->s_blocksize to BOOGAFS_BLOCK_SIZE, which is 4KB, as defined in booga.h */
    sb->s_maxbytes = BOOGAFS_MAX_FILESIZE; /* as of now, we only use 12 direct pointers, thus the max file size is 12*4K=48KB */
    sb->s_op = &boogafs_super_ops; /* install super block operation callbacks, including put_super, alloc_inode, destroy_inode, write_inode, sync_fc, statfs. */

    pr_info("fill super checker 1\n");
    /* Read sb from disk */
    bh = sb_bread(sb, 0); /* read block 0, which in our case is the super block, from the disk */
    if (!bh)
        return -EIO;

    pr_info("fill super checker 2.0\n");
    csb = (struct boogafs_sb_info *) bh->b_data; /* sb_bread() reads the block and stores the data in bh->b_data */

    pr_info("fill super checker 2.1\n");
    /* Check magic number */
    if (csb->magic != sb->s_magic) { /* both struct boogafs_sb_info and struct super_block have this magic field */
        pr_err("Wrong magic number\n");
        ret = -EINVAL;
        goto release;
    }

    pr_info("fill super checker 3\n");
    /* Alloc sb_info */
    sbi = kzalloc(sizeof(struct boogafs_sb_info), GFP_KERNEL);
    if (!sbi) {
        ret = -ENOMEM;
        goto release;
    }

    sbi->magic = csb->magic;
    sbi->nr_blocks = csb->nr_blocks; /* whatever we read from the disk is now stored in csb, and we then copy each of its field into our sbi struct, which is a pointer points to some memory. */
    sbi->nr_inodes = csb->nr_inodes;
    sbi->nr_itable_blocks = csb->nr_itable_blocks;
    sbi->nr_ibitmap_blocks = csb->nr_ibitmap_blocks;
    sbi->nr_dbitmap_blocks = csb->nr_dbitmap_blocks;
    sbi->nr_free_inodes = csb->nr_free_inodes;
    sbi->nr_free_blocks = csb->nr_free_blocks;
    sb->s_fs_info = sbi; /* in struct super_block, there is "void  *s_fs_info;" commented as "filesystem private info" */

    brelse(bh); /* decrement a buffer_head's reference count */

    pr_info("fill super checker 4\n");
    /* Alloc and copy inode_bitmap */
    sbi->inode_bitmap =
        kzalloc(sbi->nr_ibitmap_blocks * BOOGAFS_BLOCK_SIZE, GFP_KERNEL);
    if (!sbi->inode_bitmap) {
        ret = -ENOMEM;
        goto free_sbi;
    }

    pr_info("fill super checker 5\n");
    for (i = 0; i < sbi->nr_ibitmap_blocks; i++) {
        /* int idx = sbi->nr_itable_blocks + i + 1; */
        int idx = i + 1; /* unlike simplefs, in boogafs, the inode bitmap is right after the super block) */

        bh = sb_bread(sb, idx);
        if (!bh) {
            ret = -EIO;
            goto free_ifree;
        }

        memcpy((void *) sbi->inode_bitmap + i * BOOGAFS_BLOCK_SIZE, bh->b_data,
               BOOGAFS_BLOCK_SIZE);

        brelse(bh);
    }

    pr_info("fill super checker 6\n");
    /* Alloc and copy data_bitmap */
    sbi->data_bitmap =
        kzalloc(sbi->nr_dbitmap_blocks * BOOGAFS_BLOCK_SIZE, GFP_KERNEL);
    if (!sbi->data_bitmap) {
        ret = -ENOMEM;
        goto free_ifree;
    }

    pr_info("fill super checker 7\n");
    for (i = 0; i < sbi->nr_dbitmap_blocks; i++) {
        /* int idx = sbi->nr_itable_blocks + sbi->nr_ibitmap_blocks + i + 1; */
        int idx = sbi->nr_ibitmap_blocks + i + 1; /* unlike simplefs, in boogafs, the data bitmap is right after the inode bitmap block */

        bh = sb_bread(sb, idx);
        if (!bh) {
            ret = -EIO;
            goto free_bfree;
        }

        memcpy((void *) sbi->data_bitmap + i * BOOGAFS_BLOCK_SIZE, bh->b_data,
               BOOGAFS_BLOCK_SIZE);

        brelse(bh);
    }

    pr_info("create root inode...\n");
    /* Create root inode */
    root_inode = boogafs_iget(sb, BOOGAFS_ROOT_INO); /* inode number can not be zero: it seems that VFS consider 0 as an invalid inode number; thus here we use 2. */
    if (IS_ERR(root_inode)) {
        ret = PTR_ERR(root_inode);
        goto free_bfree;
    }
    pr_info("init root inode...\n");
    inode_init_owner(root_inode, NULL, root_inode->i_mode);
    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto iput;
    }

    pr_info("super block filled\n");

    return 0;

iput:
    iput(root_inode);
free_bfree:
    kfree(sbi->data_bitmap);
free_ifree:
    kfree(sbi->inode_bitmap);
free_sbi:
    kfree(sbi);
release:
    brelse(bh);

    return ret;
}