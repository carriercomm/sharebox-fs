/*
 * Operations that happen in "/"
 */

#include "slash.h"
#include "git-annex.h"

// TODO: fix the errnos (save them as soon as they happen)

/*
 * Helpers
 */

static void fullpath(char fpath[FILENAME_MAX], const char *path)
{
    strcpy(fpath, sharebox.reporoot);
    strncat(fpath, "/files", FILENAME_MAX);
    strncat(fpath, path, FILENAME_MAX);
}

static int ondisk(const char *lnk)
{
    struct stat st;
    return (stat(lnk, &st) != -1);
}

/*
 * FS Operations
 */

static int slash_getattr(const char *path, struct stat *stbuf)
{
    int res;
    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    res = lstat(fpath, stbuf);
    if (git_annexed(sharebox.reporoot, fpath)) {
        if (ondisk(fpath)) {
            res = stat(fpath, stbuf);
        } else {
            stbuf->st_mode &= ~S_IFMT;
            stbuf->st_mode |= S_IFREG; /* fake regular file */
            stbuf->st_size = 0;        /* fake size = 0 */
        }
        stbuf->st_mode |= S_IWUSR;     /* fake writable */
    }
    if (res == -1)
        return -errno;

    return 0;
}

static int slash_access(const char *path, int mask)
{
    int res;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    if (git_annexed(sharebox.reporoot, fpath)) {
        if (ondisk(fpath))
            res = access(fpath, mask & ~W_OK);
        else
            res = -EACCES;
    }
    else
        res = access(fpath, mask);

    if (res == -1)
        res = -errno;

    return res;
}

static int slash_readlink(const char *path, char *buf, size_t size)
{
    int res;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    res = readlink(fpath, buf, size - 1);
    if (res == -1)
        return -errno;

    buf[res] = '\0';
    return 0;
}


static int slash_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
               off_t offset, struct fuse_file_info *fi)
{
    DIR *dp;
    struct dirent *de;
    (void) offset;
    (void) fi;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    dp = opendir(fpath);
    if (dp == NULL)
        return -errno;

    while ((de = readdir(dp)) != NULL) {
        if (filler(buf, de->d_name, NULL, 0))
            break;
    }
    closedir(dp);

    /* We then list conflicting files */
    /*
    namelist *branch, *b;
    branch = git_branches(sharebox.reporoot);
    for (b = branch; b != NULL; b = b->next) {
        namelist *files, *f;
        files = conflicting_files(sharebox.reporoot, fpath, b->name);
        for (f = files; f != NULL; f = f->next) {
            char name[FILENAME_MAX];
            strncpy(name, ".", FILENAME_MAX);
            strncat(name, branch->name, FILENAME_MAX);
            strncat(name, ".", FILENAME_MAX);
            strncat(name, f->name, FILENAME_MAX);
            strncat(name, ".conflict", FILENAME_MAX);
            if (filler(buf, name, NULL, 0))
                break;
        }
        free_namelist(files);
    }
    free_namelist(branch);
    */

    return 0;
}

static int slash_mknod(const char *path, mode_t mode, dev_t rdev)
{
    pthread_mutex_lock(&sharebox.rwlock);

    int res;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    /* On Linux this could just be 'mknod(path, mode, rdev)' but this
       is more portable */
    if (S_ISREG(mode)) {
        res = open(fpath, O_CREAT | O_EXCL | O_WRONLY, mode);
        if (res >= 0)
            res = close(res);
    } else if (S_ISFIFO(mode))
        res = mkfifo(fpath, mode);
    else
        res = mknod(fpath, mode, rdev);

    pthread_mutex_unlock(&sharebox.rwlock);

    if (res == -1)
        return -errno;
    return 0;
}

static int slash_mkdir(const char *path, mode_t mode)
{
    int res;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    res = mkdir(fpath, mode);

    if (res == -1)
        return -errno;
    return 0;
}

static int slash_unlink(const char *path)
{
    pthread_mutex_lock(&sharebox.rwlock);

    int res;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    res = unlink(fpath);

    if (!git_ignored(sharebox.reporoot, fpath)){
        git_rm(sharebox.reporoot, fpath);
        git_commit(sharebox.reporoot, "removed %s", path + 1);
    }

    pthread_mutex_unlock(&sharebox.rwlock);

    if (res == -1)
        return -errno;

    return 0;
}

static int slash_rmdir(const char *path)
{
    int res;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    res = rmdir(fpath);
    if (res == -1)
        return -errno;

    return 0;
}

static int slash_symlink(const char *target, const char *linkname)
{
    pthread_mutex_lock(&sharebox.rwlock);

    int res;

    char flinkname[FILENAME_MAX];
    fullpath(flinkname, linkname);

    res = symlink(target, flinkname);

    if (!git_ignored(sharebox.reporoot, flinkname)){
        git_add(sharebox.reporoot, flinkname);
        git_commit(sharebox.reporoot, "created symlink %s->%s", linkname + 1, target);
    }

    pthread_mutex_unlock(&sharebox.rwlock);

    if (res == -1)
        return -errno;

    return 0;
}

static int slash_rename(const char *from, const char *to)
{
    pthread_mutex_lock(&sharebox.rwlock);

    int res;
    bool from_ignored;
    bool to_ignored;

    char ffrom[FILENAME_MAX];
    char fto[FILENAME_MAX];

    fullpath(ffrom, from);
    fullpath(fto, to);

    /* proceed to rename */
    from_ignored = git_ignored(sharebox.reporoot, ffrom);
    res = rename(ffrom, fto);
    to_ignored = git_ignored(sharebox.reporoot, fto);

    if (res != -1) {
        /* moved ignored to ignored (nothing) */

        /* moved ignored to non ignored*/
        if (from_ignored && !to_ignored){
            git_annex_add(sharebox.reporoot, fto);
            git_add(sharebox.reporoot, fto); /* this ensures links will be added too */
        }
        /* moved non ignored to ignored */
        if (!from_ignored && to_ignored){
            git_rm(sharebox.reporoot, ffrom);
        }
        /* moved non ignored to non ignored */
        if (!from_ignored && !to_ignored){
            git_mv(sharebox.reporoot, ffrom, fto);
        }

        git_commit(sharebox.reporoot, "moved %s to %s", from+1, to+1);
    }

    pthread_mutex_unlock(&sharebox.rwlock);

    if (res == -1)
        return -errno;

    return 0;
}

static int slash_chmod(const char *path, mode_t mode)
{
    pthread_mutex_lock(&sharebox.rwlock);

    int res;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    git_annex_unlock(sharebox.reporoot, fpath);

    res = chmod(fpath, mode);

    git_annex_add(sharebox.reporoot, fpath);
    git_commit(sharebox.reporoot, "chmoded %s to %o", path+1, mode);

    pthread_mutex_unlock(&sharebox.rwlock);

    if (res == -1)
        return -errno;

    return 0;
}

static int slash_chown(const char *path, uid_t uid, gid_t gid)
{
    pthread_mutex_lock(&sharebox.rwlock);

    int res;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    git_annex_unlock(sharebox.reporoot, fpath);

    res = lchown(fpath, uid, gid);

    git_annex_add(sharebox.reporoot, fpath);
    git_commit(sharebox.reporoot, "chmown on %s", path+1);

    pthread_mutex_unlock(&sharebox.rwlock);

    if (res == -1)
        return -errno;

    return 0;
}

static int slash_truncate(const char *path, off_t size)
{
    pthread_mutex_lock(&sharebox.rwlock);

    int res;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    git_annex_unlock(sharebox.reporoot, fpath);

    res = truncate(fpath, size);

    git_annex_add(sharebox.reporoot, fpath);
    git_commit(sharebox.reporoot, "truncated on %s", path+1);

    pthread_mutex_unlock(&sharebox.rwlock);

    if (res == -1)
        return -errno;

    return 0;
}

static int slash_utimens(const char *path, const struct timespec ts[2])
{
    pthread_mutex_lock(&sharebox.rwlock);

    int res;
    struct timeval tv[2];
    char fpath[FILENAME_MAX];

    tv[0].tv_sec = ts[0].tv_sec;
    tv[0].tv_usec = ts[0].tv_nsec / 1000;
    tv[1].tv_sec = ts[1].tv_sec;
    tv[1].tv_usec = ts[1].tv_nsec / 1000;

    fullpath(fpath, path);

    git_annex_unlock(sharebox.reporoot, fpath);

    res = utimes(fpath, tv);

    git_annex_add(sharebox.reporoot, fpath);
    git_commit(sharebox.reporoot, "utimens on %s", path+1);

    pthread_mutex_unlock(&sharebox.rwlock);

    if (res == -1)
        return -errno;

    return 0;
}

static int slash_open(const char *path, struct fuse_file_info *fi)
{
    int res;
    int flags;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    flags=fi->flags;

    if (git_annexed(sharebox.reporoot, fpath))
        /* Get the file on the fly, remove W_OK if it was requested */
        if (!ondisk(fpath))
            git_annex_get(sharebox.reporoot, fpath, NULL);
        if (!ondisk(fpath))
            return -EACCES;
        flags &= ~W_OK;

    res = open(fpath, flags);

    if (res == 1)
        return -errno;

    close(res);

    return 0;
}

static int slash_read(const char *path, char *buf, size_t size, off_t offset,
            struct fuse_file_info *fi)
{
    pthread_mutex_lock(&sharebox.rwlock);

    int fd;
    int res;
    (void) fi;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    if ((fd = open(fpath, O_RDONLY)) != -1)
        if ((res = pread(fd, buf, size, offset)) != -1)
            close(fd);

    pthread_mutex_unlock(&sharebox.rwlock);

    if (fd == -1 || res == -1)
        return -errno;
    return res;
}

static int slash_write(const char *path, const char *buf, size_t size,
             off_t offset, struct fuse_file_info *fi)
{
    pthread_mutex_lock(&sharebox.rwlock);

    int fd;
    int res;
    (void) fi;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    if (git_annexed(sharebox.reporoot, fpath))
        git_annex_unlock(sharebox.reporoot, fpath);

    if ((fd = open(fpath, O_WRONLY)) != -1)
        if((res = pwrite(fd, buf, size, offset)) != -1)
            close(fd);

    pthread_mutex_unlock(&sharebox.rwlock);

    if (fd == -1 || res == -1)
        return -errno;
    return res;
}

static int slash_release(const char *path, struct fuse_file_info *fi)
{
    pthread_mutex_lock(&sharebox.rwlock);

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    if (!git_ignored(sharebox.reporoot, fpath)){
        git_annex_add(sharebox.reporoot, fpath);
        git_commit(sharebox.reporoot, "released %s", path+1);
    }

    pthread_mutex_unlock(&sharebox.rwlock);

    return 0;
}

static int slash_statfs(const char *path, struct statvfs *stbuf)
{
    int res;

    char fpath[FILENAME_MAX];
    fullpath(fpath, path);

    res = statvfs(fpath, stbuf);
    if (res == -1)
        return -errno;

    return 0;
}

void init_slash(dir *d)
{
    strcpy(d->name, "/");
    (d->operations).getattr    = slash_getattr;
    (d->operations).access     = slash_access;
    (d->operations).readlink   = slash_readlink;
    (d->operations).readdir    = slash_readdir;
    (d->operations).mknod      = slash_mknod;
    (d->operations).mkdir      = slash_mkdir;
    (d->operations).symlink    = slash_symlink;
    (d->operations).unlink     = slash_unlink;
    (d->operations).rmdir      = slash_rmdir;
    (d->operations).rename     = slash_rename;
    (d->operations).chmod      = slash_chmod;
    (d->operations).chown      = slash_chown;
    (d->operations).truncate   = slash_truncate;
    (d->operations).utimens    = slash_utimens;
    (d->operations).open       = slash_open;
    (d->operations).read       = slash_read;
    (d->operations).write      = slash_write;
    (d->operations).release    = slash_release;
    (d->operations).statfs     = slash_statfs;
}
