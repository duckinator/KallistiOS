/* KallistiOS ##version##

   fs_ext2.c
   Copyright (C) 2012, 2013 Lawrence Sebald
*/

#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include <kos/fs.h>
#include <kos/mutex.h>
#include <kos/dbglog.h>

#include <ext2/fs_ext2.h>

#include "ext2fs.h"
#include "inode.h"
#include "directory.h"

/* For some reason, Newlib doesn't seem to define this function in stdlib.h. */
extern char *realpath(const char *, const char *);

#define MAX_EXT2_FILES 16

typedef struct fs_ext2_fs {
    LIST_ENTRY(fs_ext2_fs) entry;

    vfs_handler_t *vfsh;
    ext2_fs_t *fs;
    uint32_t mount_flags;
} fs_ext2_fs_t;

LIST_HEAD(ext2_list, fs_ext2_fs);
static struct ext2_list ext2_fses;
static mutex_t ext2_mutex;

static struct {
    uint32_t inode_num;
    int mode;
    uint64_t ptr;
    dirent_t dent;
    ext2_inode_t *inode;
    fs_ext2_fs_t *fs;
} fh[MAX_EXT2_FILES];

static void *fs_ext2_open(vfs_handler_t *vfs, const char *fn, int mode) {
    file_t fd;
    fs_ext2_fs_t *mnt = (fs_ext2_fs_t *)vfs->privdata;
    int rv;

    /* We don't support writing for now... */
    if((mode & (O_WRONLY | O_TRUNC))) {
        errno = EROFS;
        return NULL;
    }

    /* Find a free file handle */
    mutex_lock(&ext2_mutex);

    for(fd = 0; fd < MAX_EXT2_FILES; ++fd) {
        if(fh[fd].inode_num == 0) {
            fh[fd].inode_num = -1;
            break;
        }
    }

    if(fd >= MAX_EXT2_FILES) {
        errno = ENFILE;
        mutex_unlock(&ext2_mutex);
        return NULL;
    }

    /* Find the object in question */
    if((rv = ext2_inode_by_path(mnt->fs, fn, &fh[fd].inode,
                                &fh[fd].inode_num, 1, NULL))) {
        fh[fd].inode_num = 0;
        ext2_inode_put(fh[fd].inode);
        mutex_unlock(&ext2_mutex);

        if(rv == -ENOENT) {
            if(mode & O_CREAT)
                errno = EROFS;
            else
                errno = ENOENT;
        }
        else {
            errno = -rv;
        }

        return NULL;
    }

    /* Make sure we're not trying to open a directory for writing */
    if((fh[fd].inode->i_mode & EXT2_S_IFDIR) &&
       ((mode & O_WRONLY) || !(mode & O_DIR))) {
        errno = EISDIR;
        fh[fd].inode_num = 0;
        ext2_inode_put(fh[fd].inode);
        mutex_unlock(&ext2_mutex);
        return NULL;
    }

    /* Make sure if we're trying to open a directory that we have a directory */
    if((mode & O_DIR) && !(fh[fd].inode->i_mode & EXT2_S_IFDIR)) {
        errno = ENOTDIR;
        fh[fd].inode_num = 0;
        ext2_inode_put(fh[fd].inode);
        mutex_unlock(&ext2_mutex);
        return NULL;
    }

    /* Fill in the rest of the handle */
    fh[fd].mode = mode;
    fh[fd].ptr = 0;
    fh[fd].fs = mnt;
    mutex_unlock(&ext2_mutex);

    return (void *)(fd + 1);
}

static void fs_ext2_close(void *h) {
    file_t fd = ((file_t)h) - 1;

    mutex_lock(&ext2_mutex);

    if(fd < MAX_EXT2_FILES && fh[fd].mode) {
        ext2_inode_put(fh[fd].inode);
        fh[fd].inode_num = 0;
        fh[fd].mode = 0;

        /* This will require more work probably when we support writing, but for
           now this is enough... */
    }

    mutex_unlock(&ext2_mutex);
}

static ssize_t fs_ext2_read(void *h, void *buf, size_t cnt) {
    file_t fd = ((file_t)h) - 1;
    ext2_fs_t *fs;
    uint32_t bs, lbs, bo;
    uint8_t *block;
    uint8_t *bbuf = (uint8_t *)buf;
    ssize_t rv;

    mutex_lock(&ext2_mutex);

    /* Check that the fd is valid */
    if(fd >= MAX_EXT2_FILES || !fh[fd].inode_num || (fh[fd].mode & O_DIR)) {
        mutex_unlock(&ext2_mutex);
        errno = EINVAL;
        return -1;
    }

    /* Do we have enough left? */
    if((fh[fd].ptr + cnt) > fh[fd].inode->i_size)
        cnt = fh[fd].inode->i_size - fh[fd].ptr;

    fs = fh[fd].fs->fs;
    bs = ext2_block_size(fs);
    lbs = ext2_log_block_size(fs);
    rv = (ssize_t)cnt;
    bo = fh[fd].ptr & ((1 << lbs) - 1);

    /* Handle the first block specially if we are offset within it. */
    if(bo) {
        if(!(block = ext2_inode_read_block(fs, fh[fd].inode, fh[fd].ptr >> lbs,
                                           NULL, &errno))) {
            mutex_unlock(&ext2_mutex);
            return -1;
        }
        
        if(cnt > bs - bo) {
            memcpy(bbuf, block + bo, bs - bo);
            fh[fd].ptr += bs - bo;
            cnt -= bs - bo;
            bbuf += bs - bo;
        }
        else {
            memcpy(bbuf, block + bo, cnt);
            fh[fd].ptr += cnt;
            cnt = 0;
        }
    }

    /* While we still have more to read, do it. */
    while(cnt) {
        if(!(block = ext2_inode_read_block(fs, fh[fd].inode, fh[fd].ptr >> lbs,
                                           NULL, &errno))) {
            mutex_unlock(&ext2_mutex);
            return -1;
        }

        if(cnt > bs) {
            memcpy(bbuf, block, bs);
            fh[fd].ptr += bs;
            cnt -= bs;
            bbuf += bs;
        }
        else {
            memcpy(bbuf, block, cnt);
            fh[fd].ptr += cnt;
            cnt = 0;
        }
    }

    /* We're done, clean up and return. */
    mutex_unlock(&ext2_mutex);
    return rv;
}

static off_t fs_ext2_seek(void *h, off_t offset, int whence) {
    file_t fd = ((file_t)h) - 1;
    off_t rv;

    mutex_lock(&ext2_mutex);

    /* Check that the fd is valid */
    if(fd >= MAX_EXT2_FILES || !fh[fd].inode_num || (fh[fd].mode & O_DIR)) {
        mutex_unlock(&ext2_mutex);
        errno = EINVAL;
        return -1;
    }

    /* Update current position according to arguments */
    switch(whence) {
        case SEEK_SET:
            fh[fd].ptr = offset;
            break;

        case SEEK_CUR:
            fh[fd].ptr += offset;
            break;

        case SEEK_END:
            fh[fd].ptr = fh[fd].inode->i_size + offset;
            break;

        default:
            mutex_unlock(&ext2_mutex);
            return -1;
    }

    /* Check bounds */    
    if(fh[fd].ptr > fh[fd].inode->i_size) fh[fd].ptr = fh[fd].inode->i_size;

    rv = (off_t)fh[fd].ptr;
    mutex_unlock(&ext2_mutex);
    return rv;
}

static off_t fs_ext2_tell(void *h) {
    file_t fd = ((file_t)h) - 1;
    off_t rv;

    mutex_lock(&ext2_mutex);

    if(fd >= MAX_EXT2_FILES || !fh[fd].inode_num || (fh[fd].mode & O_DIR)) {
        mutex_unlock(&ext2_mutex);
        errno = EINVAL;
        return -1;
    }

    rv = (off_t)fh[fd].ptr;
    mutex_unlock(&ext2_mutex);
    return rv;
}

static size_t fs_ext2_total(void *h) {
    file_t fd = ((file_t)h) - 1;
    size_t rv;

    mutex_lock(&ext2_mutex);

    if(fd >= MAX_EXT2_FILES || !fh[fd].inode_num || (fh[fd].mode & O_DIR)) {
        mutex_unlock(&ext2_mutex);
        errno = EINVAL;
        return -1;
    }

    rv = fh[fd].inode->i_size;
    mutex_unlock(&ext2_mutex);
    return rv;
}

static dirent_t *fs_ext2_readdir(void *h) {
    file_t fd = ((file_t)h) - 1;
    ext2_fs_t *fs;
    uint32_t bs, lbs;
    uint8_t *block;
    ext2_dirent_t *dent;
    ext2_inode_t *inode;
    int err;

    mutex_lock(&ext2_mutex);

    /* Check that the fd is valid */
    if(fd >= MAX_EXT2_FILES || !fh[fd].inode_num || !(fh[fd].mode & O_DIR)) {
        mutex_unlock(&ext2_mutex);
        errno = EINVAL;
        return NULL;
    }

    fs = fh[fd].fs->fs;
    bs = ext2_block_size(fs);
    lbs = ext2_log_block_size(fs);

retry:
    /* Make sure we're not at the end of the directory */
    if(fh[fd].ptr >= fh[fd].inode->i_size) {
        mutex_unlock(&ext2_mutex);
        return NULL;
    }

    if(!(block = ext2_inode_read_block(fs, fh[fd].inode, fh[fd].ptr >> lbs,
                                       NULL, &errno))) {
        mutex_unlock(&ext2_mutex);
        return NULL;
    }

    /* Grab our directory entry from the block */
    dent = (ext2_dirent_t *)(block + (fh[fd].ptr & (bs - 1)));

    /* Make sure the directory entry is sane */
    if(!dent->rec_len) {
        mutex_unlock(&ext2_mutex);
        errno = EBADF;
        return NULL;
    }

    /* If we have a blank inode value, the entry should be skipped. */
    if(!dent->inode) {
        fh[fd].ptr += dent->rec_len;
        goto retry;
    }

    /* Grab the inode of this entry */
    if(!(inode = ext2_inode_get(fs, dent->inode, &err))) {
        mutex_unlock(&ext2_mutex);
        errno = EIO;
        return NULL;
    }
        
    /* Fill in the static directory entry. */
    fh[fd].dent.size = inode->i_size;
    memcpy(fh[fd].dent.name, dent->name, dent->name_len);
    fh[fd].dent.name[dent->name_len] = 0;
    fh[fd].dent.time = inode->i_mtime;
    fh[fd].ptr += dent->rec_len;

    /* Set the attribute bits based on the user permissions on the file. */
    if(inode->i_mode & EXT2_S_IFDIR)
        fh[fd].dent.attr = O_DIR;
    else
        fh[fd].dent.attr = 0;

    ext2_inode_put(inode);
    mutex_unlock(&ext2_mutex);
    return &fh[fd].dent;
}

static int int_rename(fs_ext2_fs_t *fs, const char *fn1, const char *fn2,
                      ext2_inode_t *pinode, ext2_inode_t *finode,
                      uint32_t finode_num, int isfile) {
    ext2_inode_t *dpinode, *dinode;
    uint32_t dpinode_num, tmp;
    char *cp, *ent;
    int irv, isdir = 0;
    ext2_dirent_t *dent;

    /* Make a writable copy of the destination filename. */
    if(!(cp = strdup(fn2))) {
        return -ENOMEM;
    }

    /* Separate our copy into the parent and the entry we want to create. */
    if(!(ent = strrchr(cp, '/'))) {
        free(cp);
        return -EINVAL;
    }

    /* Split the string. */
    *ent++ = 0;

    /* Look up the parent of the destination. */
    if((irv = ext2_inode_by_path(fs->fs, cp, &dpinode, &dpinode_num, 1,
                                 NULL))) {
        free(cp);
        return irv;
    }

    /* If the entry we get back is not a directory, then we've got problems. */
    if((dpinode->i_mode & 0xF000) != EXT2_S_IFDIR) {
        ext2_inode_put(dpinode);
        free(cp);
        return -ENOTDIR;
    }

    /* Grab the directory entry for the new filename, if it exists. */
    dent = ext2_dir_entry(fs->fs, dpinode, ent);

    /* If the entry exists, we have a bit more error checking to do. */
    if(dent) {
        if(!(dinode = ext2_inode_get(fs->fs, dent->inode, &irv))) {
            free(cp);
            ext2_inode_put(dpinode);
            return -EIO;
        }

        /* Do we have a directory? */
        if((dinode->i_mode & 0xF000) == EXT2_S_IFDIR) {
            isdir = 1;

            if(isfile) {
                /* We have a directory... Return error. */
                free(cp);
                ext2_inode_put(dinode);
                ext2_inode_put(dpinode);
                return -EISDIR;
            }
            else {
                /* We have a directory... Make sure it is empty. */
                if(!(irv = ext2_dir_is_empty(fs->fs, dinode))) {
                    free(cp);
                    ext2_inode_put(dinode);
                    ext2_inode_put(dpinode);
                    return -ENOTEMPTY;
                }
                else if(irv == -1) {
                    free(cp);
                    ext2_inode_put(dinode);
                    ext2_inode_put(dpinode);
                    return -EIO;
                }
            }
        }

        /* Make sure we don't have any open file descriptors to what will be
           replaced at the destination. */
        for(irv = 0; irv < MAX_EXT2_FILES; ++irv) {
            if(fh[irv].inode_num == dent->inode) {
                free(cp);
                ext2_inode_put(dinode);
                ext2_inode_put(dpinode);
                return -EBUSY;
            }
        }
    }

    /* Gulp... Here comes the real work... */
    if(dent) {
        /* We are overwriting something. Remove the object that we're
           overwriting from its parent. */
        if((irv = ext2_dir_rm_entry(fs->fs, dpinode, ent, &tmp))) {
            free(cp);
            ext2_inode_put(dpinode);
            ext2_inode_put(dinode);
            return -EIO;
        }

        ext2_inode_put(dinode);

        /* Decrement the old object's reference count, deallocating it if
           necessary. */
        if(ext2_inode_deref(fs->fs, tmp, isdir)) {
            free(cp);
            ext2_inode_put(dpinode);
            return -EIO;
        }

        /* If it was a directory that we just removed, then decrement the parent
           directory's reference count. */
        if(isdir) {
            --dpinode->i_links_count;
            ext2_inode_mark_dirty(dpinode);
        }
    }

    /* Add the new entry to the directory. */
    if(ext2_dir_add_entry(fs->fs, dpinode, ent, finode_num, finode, NULL)) {
        free(cp);
        ext2_inode_put(dpinode);
        return -EIO;
    }

    /* Unlink the item we're moving from its parent directory now that it is
       safely in its new home. */
    if((irv = ext2_dir_rm_entry(fs->fs, pinode, fn1, &tmp))) {
        free(cp);
        ext2_inode_put(dpinode);
        return -EIO;
    }

    /* If the thing we moved was a directory, we need to fix its '..' entry and
       update the reference counts of the inodes involved. */
    if(!isfile) {
        if((ext2_dir_redir_entry(fs->fs, finode, "..", dpinode_num, NULL)))
            return -EIO;

        --pinode->i_links_count;
        ++dpinode->i_links_count;
        ext2_inode_mark_dirty(dpinode);
        ext2_inode_mark_dirty(pinode);
    }

    free(cp);
    ext2_inode_put(dpinode);
    return 0;
}

static int fs_ext2_rename(vfs_handler_t *vfs, const char *fn1,
                          const char *fn2) {
    fs_ext2_fs_t *fs = (fs_ext2_fs_t *)vfs->privdata;
    int irv;
    ext2_inode_t *pinode, *inode;
    uint32_t inode_num;
    ext2_dirent_t *dent;
    char *cp, *ent;

    /* Make sure we get valid filenames. */
    if(!fn1 || !fn2) {
        errno = ENOENT;
        return -1;
    }

    /* No, you cannot move the root directory. */
    if(!*fn1) {
        errno = EINVAL;
        return -1;
    }

    /* Make sure the fs is writable */
    if(!(fs->mount_flags & FS_EXT2_MOUNT_READWRITE)) {
        errno = EROFS;
        return -1;
    }

    /* Make a writable copy of the source filename. */
    if(!(cp = strdup(fn1))) {
        errno = ENOMEM;
        return -1;
    }

    /* Separate our copy into the parent and the file we want to move. */
    if(!(ent = strrchr(cp, '/'))) {
        free(cp);
        errno = EINVAL;
        return -1;
    }

    /* Split the string. */
    *ent++ = 0;

    mutex_lock(&ext2_mutex);

    /* Find the parent directory of the original object.*/
    if((irv = ext2_inode_by_path(fs->fs, cp, &pinode, &inode_num, 1, NULL))) {
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = -irv;
        return -1;
    }

    /* If the entry we get back is not a directory, then we've got problems. */
    if((pinode->i_mode & 0xF000) != EXT2_S_IFDIR) {
        ext2_inode_put(pinode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = ENOTDIR;
        return -1;
    }

    /* Grab the directory entry for the old filename. */
    if(!(dent = ext2_dir_entry(fs->fs, pinode, ent))) {
        ext2_inode_put(pinode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = ENOENT;
        return -1;
    }

    /* Find the inode of the entry we want to move. */
    if(!(inode = ext2_inode_get(fs->fs, dent->inode, &irv))) {
        mutex_unlock(&ext2_mutex);
        errno = EIO;
        return -1;
    }

    /* Is it a directory? */
    if((inode->i_mode & 0xF000) == EXT2_S_IFDIR) {
        if((irv = int_rename(fs, ent, fn2, pinode, inode, dent->inode,
                             0)) < 0) {
            errno = -irv;
            irv = -1;
        }
    }
    else {
        if((irv = int_rename(fs, ent, fn2, pinode, inode, dent->inode,
                             1)) < 0) {
            errno = -irv;
            irv = -1;
        }
    }

    free(cp);
    ext2_inode_put(pinode);
    ext2_inode_put(inode);
    mutex_lock(&ext2_mutex);
    return irv;
}

static int fs_ext2_unlink(vfs_handler_t *vfs, const char *fn) {
    fs_ext2_fs_t *fs = (fs_ext2_fs_t *)vfs->privdata;
    int irv;
    ext2_inode_t *pinode, *inode;
    uint32_t inode_num;
    ext2_dirent_t *dent;
    char *cp, *ent;
    uint32_t in_num;

    /* Make sure there is a filename given */
    if(!fn) {
        errno = ENOENT;
        return -1;
    }

    /* Make sure we're not trying to remove the root of the filesystem. */
    if(!*fn) {
        errno = EPERM;
        return -1;
    }

    /* Make sure the fs is writable */
    if(!(fs->mount_flags & FS_EXT2_MOUNT_READWRITE)) {
        errno = EROFS;
        return -1;
    }

    /* Make a writable copy of the filename. */
    if(!(cp = strdup(fn))) {
        errno = ENOMEM;
        return -1;
    }

    /* Separate our copy into the parent and the file we want to remove. */
    if(!(ent = strrchr(cp, '/'))) {
        free(cp);
        errno = EPERM;
        return -1;
    }

    /* Split the string. */
    *ent++ = 0;

    mutex_lock(&ext2_mutex);

    /* Find the parent directory of the object in question.*/
    if((irv = ext2_inode_by_path(fs->fs, cp, &pinode, &inode_num, 1, NULL))) {
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = -irv;
        return -1;
    }

    /* If the entry we get back is not a directory, then we've got problems. */
    if((pinode->i_mode & 0xF000) != EXT2_S_IFDIR) {
        ext2_inode_put(pinode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = ENOTDIR;
        return -1;
    }

    /* Try to find the directory entry of the item we want to remove. */
    if(!(dent = ext2_dir_entry(fs->fs, pinode, ent))) {
        ext2_inode_put(pinode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = ENOENT;
        return -1;
    }

    /* Find the inode of the entry we want to remove. */
    if(!(inode = ext2_inode_get(fs->fs, dent->inode, &irv))) {
        ext2_inode_put(pinode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = EIO;
        return -1;
    }

    /* Make sure we don't try to remove a directory with unlink. That is what
       rmdir is for. */
    if((inode->i_mode & 0xF000) == EXT2_S_IFDIR) {
        ext2_inode_put(pinode);
        ext2_inode_put(inode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = EPERM;
        return -1;
    }

    /* Make sure we don't have any open file descriptors to the file. */
    for(irv = 0; irv < MAX_EXT2_FILES; ++irv) {
        if(fh[irv].inode_num == dent->inode) {
            ext2_inode_put(pinode);
            ext2_inode_put(inode);
            mutex_unlock(&ext2_mutex);
            free(cp);
            errno = EBUSY;
            return -1;
        }
    }

    /* Remove the entry from the parent's directory. */
    if((irv = ext2_dir_rm_entry(fs->fs, pinode, ent, &in_num))) {
        ext2_inode_put(pinode);
        ext2_inode_put(inode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = -irv;
        return -1;
    }

    /* Update the times in the parent's inode */
    pinode->i_ctime = pinode->i_mtime = time(NULL);

    /* We're done with these now, so clean them up. */
    ext2_inode_put(pinode);
    ext2_inode_put(inode);
    free(cp);

    /* Free up the inode and all the data blocks. */
    if((irv = ext2_inode_deref(fs->fs, in_num, 0))) {
        mutex_unlock(&ext2_mutex);
        errno = -irv;
        return -1;
    }

    /* And, we're done. Unlock the mutex. */
    mutex_unlock(&ext2_mutex);
    return 0;    
}

static int fs_ext2_stat(vfs_handler_t *vfs, const char *fn, stat_t *rv) {
    fs_ext2_fs_t *fs = (fs_ext2_fs_t *)vfs->privdata;
    int irv;
    ext2_inode_t *inode;
    uint32_t inode_num;

    if(!rv) {
        errno = EINVAL;
        return -1;
    }

    mutex_lock(&ext2_mutex);

    /* Find the object in question */
    if((irv = ext2_inode_by_path(fs->fs, fn, &inode, &inode_num, 1, NULL))) {
        mutex_unlock(&ext2_mutex);
        errno = -irv;
        return -1;
    }

    /* Fill in the easy parts of the structure. */
    rv->dev = vfs;
    rv->unique = inode_num;
    rv->size = inode->i_size;
    rv->time = inode->i_mtime;
    rv->attr = 0;

    /* Parse out the ext2 mode bits */
    switch(inode->i_mode & 0xF000) {
        case EXT2_S_IFLNK:
            rv->type = STAT_TYPE_SYMLINK;
            break;

        case EXT2_S_IFREG:
            rv->type = STAT_TYPE_FILE;
            break;

        case EXT2_S_IFDIR:
            rv->type = STAT_TYPE_DIR;
            break;

        case EXT2_S_IFSOCK:
        case EXT2_S_IFIFO:
        case EXT2_S_IFBLK:
        case EXT2_S_IFCHR:
            rv->type = STAT_TYPE_PIPE;
            break;

        default:
            rv->type = STAT_TYPE_NONE;
            break;
    }

    /* Set the attribute bits based on the user permissions on the file. */
    if(inode->i_mode & EXT2_S_IRUSR)
        rv->attr |= STAT_ATTR_R;
    if(inode->i_mode & EXT2_S_IWUSR)
        rv->attr |= STAT_ATTR_W;

    ext2_inode_put(inode);
    mutex_unlock(&ext2_mutex);

    return 0;
}

static int fs_ext2_mkdir(vfs_handler_t *vfs, const char *fn) {
    fs_ext2_fs_t *fs = (fs_ext2_fs_t *)vfs->privdata;
    int irv;
    ext2_inode_t *inode, *ninode;
    uint32_t inode_num, ninode_num;
    char *cp, *nd;

    /* Make sure there is a filename given */
    if(!fn) {
        errno = ENOENT;
        return -1;
    }

    /* Make sure the fs is writable */
    if(!(fs->mount_flags & FS_EXT2_MOUNT_READWRITE)) {
        errno = EROFS;
        return -1;
    }

    /* The root directory has to exist... */
    if(!*fn) {
        errno = EEXIST;
        return -1;
    }

    /* Make a writable copy of the filename */
    if(!(cp = strdup(fn))) {
        errno = ENOMEM;
        return -1;
    }

    /* Separate our copy into the parent and the directory we want to create */
    if(!(nd = strrchr(cp, '/'))) {
        free(cp);
        errno = ENOENT;
        return -1;
    }

    /* Split the string. */
    *nd++ = 0;

    mutex_lock(&ext2_mutex);

    /* Find the parent of the directory we want to create. */
    if((irv = ext2_inode_by_path(fs->fs, cp, &inode, &inode_num, 1, NULL))) {
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = -irv;
        return -1;
    }

    /* See if the directory contains the item we want to create */
    if(ext2_dir_entry(fs->fs, inode, nd)) {
        ext2_inode_put(inode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = EEXIST;
        return -1;
    }

    /* Allocate a new inode for the new directory. */
    if(!(ninode = ext2_inode_alloc(fs->fs, inode_num, &irv, &ninode_num))) {
        ext2_inode_put(inode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = irv;
        return -1;
    }

    /* Fill in the inode. Copy most of the interesting parts from the parent. */
    ninode->i_mode = inode->i_mode;
    ninode->i_uid = inode->i_uid;
    ninode->i_atime = ninode->i_ctime = ninode->i_mtime = time(NULL);
    ninode->i_gid = inode->i_gid;
    ninode->i_osd2.l_i_uid_high = inode->i_osd2.l_i_uid_high;
    ninode->i_osd2.l_i_gid_high = inode->i_osd2.l_i_gid_high;

    /* Fill in the directory data for the new directory. */
    if((irv = ext2_dir_create_empty(fs->fs, ninode, ninode_num, inode_num))) {
        ext2_inode_put(inode);
        ext2_inode_deref(fs->fs, ninode_num, 1);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = -irv;
        return -1;
    }

    /* Add an entry to the parent directory. */
    if((irv = ext2_dir_add_entry(fs->fs, inode, nd, ninode_num, ninode,
                                 NULL))) {
        ext2_inode_put(inode);
        ext2_inode_deref(fs->fs, ninode_num, 1);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = -irv;
        return -1;
    }

    /* Increment the parent directory's link count. */
    ++inode->i_links_count;
    ext2_inode_mark_dirty(inode);

    ext2_inode_put(ninode);
    ext2_inode_put(inode);
    mutex_unlock(&ext2_mutex);
    free(cp);
    return 0;
}

static int fs_ext2_rmdir(vfs_handler_t *vfs, const char *fn) {
    fs_ext2_fs_t *fs = (fs_ext2_fs_t *)vfs->privdata;
    int irv;
    ext2_inode_t *pinode, *inode;
    uint32_t inode_num;
    ext2_dirent_t *dent;
    char *cp, *ent;
    uint32_t in_num;

    /* Make sure there is a filename given */
    if(!fn) {
        errno = ENOENT;
        return -1;
    }

    /* Make sure we're not trying to remove the root of the filesystem. */
    if(!*fn || (fn[0] == '/' && !fn[1])) {
        errno = EPERM;
        return -1;
    }

    /* Make sure the fs is writable */
    if(!(fs->mount_flags & FS_EXT2_MOUNT_READWRITE)) {
        errno = EROFS;
        return -1;
    }

    /* Make a writable copy of the filename. */
    if(!(cp = strdup(fn))) {
        errno = ENOMEM;
        return -1;
    }

    /* Separate our copy into the parent and the file we want to remove. */
    if(!(ent = strrchr(cp, '/'))) {
        free(cp);
        errno = EPERM;
        return -1;
    }

    /* Split the string. */
    *ent++ = 0;

    mutex_lock(&ext2_mutex);

    /* Find the parent directory of the object in question.*/
    if((irv = ext2_inode_by_path(fs->fs, cp, &pinode, &inode_num, 1, NULL))) {
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = -irv;
        return -1;
    }

    /* If the entry we get back is not a directory, then we've got problems. */
    if((pinode->i_mode & 0xF000) != EXT2_S_IFDIR) {
        ext2_inode_put(pinode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = ENOTDIR;
        return -1;
    }

    /* Try to find the directory entry of the item we want to remove. */
    if(!(dent = ext2_dir_entry(fs->fs, pinode, ent))) {
        ext2_inode_put(pinode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = ENOENT;
        return -1;
    }

    /* Find the inode of the entry we want to remove. */
    if(!(inode = ext2_inode_get(fs->fs, dent->inode, &irv))) {
        ext2_inode_put(pinode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = EIO;
        return -1;
    }

    /* Make sure we don't try to remove a non-directory with rmdir. That is what
       unlink is for. */
    if((inode->i_mode & 0xF000) != EXT2_S_IFDIR) {
        ext2_inode_put(pinode);
        ext2_inode_put(inode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = EPERM;
        return -1;
    }

    /* Make sure we don't have any open file descriptors to the directory. */
    for(irv = 0; irv < MAX_EXT2_FILES; ++irv) {
        if(fh[irv].inode_num == dent->inode) {
            ext2_inode_put(pinode);
            ext2_inode_put(inode);
            mutex_unlock(&ext2_mutex);
            free(cp);
            errno = EBUSY;
            return -1;
        }
    }

    /* Remove the entry from the parent's directory. */
    if((irv = ext2_dir_rm_entry(fs->fs, pinode, ent, &in_num))) {
        ext2_inode_put(pinode);
        ext2_inode_put(inode);
        mutex_unlock(&ext2_mutex);
        free(cp);
        errno = -irv;
        return -1;
    }

    /* We're done with these now, so clean them up. */
    ext2_inode_put(inode);
    free(cp);

    /* Free up the inode and all the data blocks. */
    if((irv = ext2_inode_deref(fs->fs, in_num, 1))) {
        mutex_unlock(&ext2_mutex);
        errno = -irv;
        return -1;
    }

    /* Decrement the reference count on the parent's inode and update the times
       stored in it. */
    pinode->i_ctime = pinode->i_mtime = time(NULL);
    --pinode->i_links_count;
    ext2_inode_mark_dirty(pinode);
    ext2_inode_put(pinode);

    /* And, we're done. Unlock the mutex. */
    mutex_unlock(&ext2_mutex);
    return 0;
}

static int fs_ext2_fcntl(void *h, int cmd, va_list ap) {
    file_t fd = ((file_t)h) - 1;
    int rv = -1;

    (void)ap;

    mutex_lock(&ext2_mutex);

    if(fd >= MAX_EXT2_FILES || !fh[fd].inode_num) {
        mutex_unlock(&ext2_mutex);
        errno = EBADF;
        return -1;
    }

    switch(cmd) {
        case F_GETFL:
            rv = fh[fd].mode;
            break;

        case F_SETFL:
        case F_GETFD:
        case F_SETFD:
            rv = 0;
            break;

        default:
            errno = EINVAL;
    }

    mutex_unlock(&ext2_mutex);
    return rv;
}

/* This is a template that will be used for each mount */
static vfs_handler_t vh = {
    /* Name Handler */
    {
        { 0 },                  /* name */
        0,                      /* in-kernel */
        0x00010000,             /* Version 1.0 */
        NMMGR_FLAGS_NEEDSFREE,  /* We malloc each VFS struct */
        NMMGR_TYPE_VFS,         /* VFS handler */
        NMMGR_LIST_INIT         /* list */
    },

    0, NULL,                    /* no cacheing, privdata */

    fs_ext2_open,               /* open */
    fs_ext2_close,              /* close */
    fs_ext2_read,               /* read */
    NULL,                       /* write */
    fs_ext2_seek,               /* seek */
    fs_ext2_tell,               /* tell */
    fs_ext2_total,              /* total */
    fs_ext2_readdir,            /* readdir */
    NULL,                       /* ioctl */
    fs_ext2_rename,             /* rename */
    fs_ext2_unlink,             /* unlink */
    NULL,                       /* mmap */
    NULL,                       /* complete */
    fs_ext2_stat,               /* stat */
    fs_ext2_mkdir,              /* mkdir */
    fs_ext2_rmdir,              /* rmdir */
    fs_ext2_fcntl,              /* fcntl */
    NULL                        /* poll */
};

static int initted = 0;

/* These two functions borrow heavily from the same functions in fs_romdisk */
int fs_ext2_mount(const char *mp, kos_blockdev_t *dev, uint32_t flags) {
    ext2_fs_t *fs;
    fs_ext2_fs_t *mnt;
    vfs_handler_t *vfsh;

    if(!initted)
        return -1;

    if((flags & FS_EXT2_MOUNT_READWRITE) && !dev->write_blocks) {
        dbglog(DBG_DEBUG, "fs_ext2: device does not support writing, cannot "
               "mount filesystem as read-write\n");
        return -1;
    }

    mutex_lock(&ext2_mutex);

    /* Try to initialize the filesystem */
    if(!(fs = ext2_fs_init(dev, flags))) {
        mutex_unlock(&ext2_mutex);
        dbglog(DBG_DEBUG, "fs_ext2: device does not contain a valid ext2fs.\n");
        return -1;
    }

    /* Create a mount structure */
    if(!(mnt = (fs_ext2_fs_t *)malloc(sizeof(fs_ext2_fs_t)))) {
        dbglog(DBG_DEBUG, "fs_ext2: out of memory creating fs structure\n");
        ext2_fs_shutdown(fs);
        mutex_unlock(&ext2_mutex);
        return -1;
    }

    mnt->fs = fs;
    mnt->mount_flags = flags;

    /* Create a VFS structure */
    if(!(vfsh = (vfs_handler_t *)malloc(sizeof(vfs_handler_t)))) {
        dbglog(DBG_DEBUG, "fs_ext2: out of memory creating vfs handler\n");
        free(mnt);
        ext2_fs_shutdown(fs);
        mutex_unlock(&ext2_mutex);
        return -1;
    }

    memcpy(vfsh, &vh, sizeof(vfs_handler_t));
    strcpy(vfsh->nmmgr.pathname, mp);
    vfsh->privdata = mnt;
    mnt->vfsh = vfsh;

    /* Add it to our list */
    LIST_INSERT_HEAD(&ext2_fses, mnt, entry);

    /* Register with the VFS */
    if(nmmgr_handler_add(&vfsh->nmmgr)) {
        dbglog(DBG_DEBUG, "fs_ext2: couldn't add fs to nmmgr\n");
        free(vfsh);
        free(mnt);
        ext2_fs_shutdown(fs);
        mutex_unlock(&ext2_mutex);
        return -1;
    }

    mutex_unlock(&ext2_mutex);
    return 0;
}

int fs_ext2_unmount(const char *mp) {
    fs_ext2_fs_t *i;
    int found = 0, rv = 0;

    /* Find the fs in question */
    mutex_lock(&ext2_mutex);
    LIST_FOREACH(i, &ext2_fses, entry) {
        if(!strcmp(mp, i->vfsh->nmmgr.pathname)) {
            found = 1;
            break;
        }
    }

    if(found) {
        LIST_REMOVE(i, entry);

        /* XXXX: We should probably do something with open files... */
        nmmgr_handler_remove(&i->vfsh->nmmgr);
        free(i->vfsh);
        free(i);
    }
    else {
        errno = ENOENT;
        rv = -1;
    }

    mutex_unlock(&ext2_mutex);
    return rv;
}

int fs_ext2_init(void) {
    if(initted)
        return 0;

    LIST_INIT(&ext2_fses);
    mutex_init(&ext2_mutex, MUTEX_TYPE_NORMAL);
    initted = 1;

    memset(fh, 0, sizeof(fh));

    return 0;
}

int fs_ext2_shutdown(void) {
    fs_ext2_fs_t *i, *next;

    if(!initted)
        return 0;

    /* Clean up the mounted filesystems */
    i = LIST_FIRST(&ext2_fses);
    while(i) {
        next = LIST_NEXT(i, entry);

        /* XXXX: We should probably do something with open files... */
        nmmgr_handler_remove(&i->vfsh->nmmgr);
        free(i->vfsh);
        free(i);

        i = next;
    }

    mutex_destroy(&ext2_mutex);
    initted = 0;

    return 0;
}
