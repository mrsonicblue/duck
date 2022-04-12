#define FUSE_USE_VERSION 26

#include <config.h>

#define _XOPEN_SOURCE 700

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <path.h>

#define BUFFER_SIZE 4096
#define CD_SECTOR_LEN 2352
#define MAP_SECTOR_LEN (CD_SECTOR_LEN * 8)

struct FileInfo
{
    int fd;
    char *map;
    int mapsize;
    char *path;
};

#define GET_FILEINFO(fi) (struct FileInfo *)(uintptr_t)fi->fh

static char *_mountpath;
static ino_t _mountino;
static char *_srcpath;
static char *_corename;

int timeval_subtract (struct timeval *result, struct timeval *x, struct timeval *y)
{
    /* Perform the carry for the later subtraction by updating y. */
    if (x->tv_usec < y->tv_usec) {
        int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
        y->tv_usec -= 1000000 * nsec;
        y->tv_sec += nsec;
    }
    if (x->tv_usec - y->tv_usec > 1000000) {
        int nsec = (x->tv_usec - y->tv_usec) / 1000000;
        y->tv_usec += 1000000 * nsec;
        y->tv_sec -= nsec;
    }

    /* Compute the time remaining to wait.
        tv_usec is certainly positive. */
    result->tv_sec = x->tv_sec - y->tv_sec;
    result->tv_usec = x->tv_usec - y->tv_usec;

    /* Return 1 if result is negative. */
    return x->tv_sec < y->tv_sec;
}

static int duck_getattr_path(const char *path, struct stat *stbuf)
{
    int res;
	if ((res = lstat(path, stbuf)) == -1)
		return -errno;

    return 0;
}

static int duck_getattr(const char *path, struct stat *stbuf)
{
    printf("duck_getattr: %s\n", path);

    char *abspath = pathjoin(_srcpath, path);
    int res = duck_getattr_path(abspath, stbuf);
    free(abspath);

    return res;
}

static void duck_readdir_path(const char *path, void *buf, fuse_fill_dir_t filler)
{
    DIR *dp;
	if ((dp = opendir(path)) == NULL)
		return;
    
	struct dirent *de;
	while ((de = readdir(dp)) != NULL)
    {
        // Prevent recursion by omitting mount directory
        if (de->d_ino == _mountino)
            continue;

        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;

        if (filler(buf, de->d_name, &st, 0))
            break;
	}

    closedir(dp);
}

static int duck_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    printf("duck_readdir: %s\n", path);

	(void) offset;
	(void) fi;

    char *abspath = pathjoin(_srcpath, path);
    duck_readdir_path(abspath, buf, filler);
    free(abspath);

    return 0;
}

static void duck_make_mappath(char *buf, const char *path)
{
    sprintf(buf, "%s.duckmap", path);
}

static void duck_readmap(const char *path, struct FileInfo *fh)
{
    printf("Checking for existing map file\n");

    struct stat st;
    if (stat(path, &st) == -1)
    {
        printf("No existing map file\n");
        return;
    }

    printf("File size is %llu\n", st.st_size);

    if (st.st_size != fh->mapsize)
    {
        printf("File and map sizes are mismatched - not reading\n");
        return;
    }

    printf("Reading map\n");

    int fd;
    if ((fd = open(path, O_RDONLY)) == -1)
    {
        printf("Failed to open map file\n");
        return;
    }

    if (pread(fd, fh->map, fh->mapsize, 0) == -1)
    {
        printf("Failed to read map file\n");

        // Erase map - just in case it was partially read
        memset(fh->map, 0, fh->mapsize);
    }

    close(fd);
}

static int duck_open_path(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int fd;
    if ((fd = open(path, fi->flags, mode)) == -1)
    //if ((fd = open(path, O_RDONLY)) == -1)
        return -errno;

    struct FileInfo *fh = malloc(sizeof(struct FileInfo));
    memset(fh, 0, sizeof(struct FileInfo));

    fh->fd = fd;
    fh->path = malloc(strlen(path) + 1);
    strcpy(fh->path, path);

    if (pathhasext(path, ".bin"))
    {
        printf("Setting up map\n");

        struct stat st;
        stat(path, &st);
        printf("File size is %llu\n", st.st_size);

        int mapsize = st.st_size / MAP_SECTOR_LEN;
        if (mapsize % MAP_SECTOR_LEN)
            mapsize++;

        printf("Map size is %d\n", mapsize);

        fh->map = malloc(mapsize);
        memset(fh->map, 0, mapsize);
        fh->mapsize = mapsize;

        char mappath[BUFFER_SIZE];
        duck_make_mappath(mappath, path);
        duck_readmap(mappath, fh);

        // Set inbound file to direct i/o to disable readahead cache
        fi->direct_io = 1;
    }

    fi->fh = (uintptr_t)fh;

    return 0;
}

static int duck_open(const char *path, struct fuse_file_info *fi)
{
    printf("duck_open: %s\n", path);

    char *abspath = pathjoin(_srcpath, path);
    int res = duck_open_path(abspath, 0, fi);
    free(abspath);

    return res;
}

static int duck_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    //printf("duck_read: %s\n", path);
    //printf("duck_read: %lld - %u\n", offset, size);
    
    (void) path;
    struct FileInfo *fh = GET_FILEINFO(fi);

    struct timeval start;
    struct timeval end;
    struct timeval diff;
    gettimeofday(&start, NULL);

    int res;
    if ((res = pread(fh->fd, buf, size, offset)) == -1)
        res = -errno;

    gettimeofday(&end, NULL);
    timeval_subtract(&diff, &end, &start);
    printf("d: %ld.%06ld\n", diff.tv_sec, diff.tv_usec);
    
    if (fh->map)
    {
        int index = offset / MAP_SECTOR_LEN;
        int bit = (offset / CD_SECTOR_LEN) % 8;

        fh->map[index] = fh->map[index] | ('\1' << bit);
    }

    return res;
}

static void duck_writemap(const char *path, struct FileInfo *fh)
{
    printf("Writing map\n");

    int fd;
    if ((fd = open(path, O_WRONLY | O_TRUNC | O_CREAT, 0644)) == -1)
    {
        printf("Failed to open map file\n");
        return;
    }

    if (pwrite(fd, fh->map, fh->mapsize, 0) == -1)
        printf("Failed to write to map file\n");

    close(fd);
}

static int duck_release_path(const char *path, struct fuse_file_info *fi)
{
    struct FileInfo *fh = GET_FILEINFO(fi);

    close(fh->fd);

    if (fh->map)
    {
        char mappath[BUFFER_SIZE];
        duck_make_mappath(mappath, path);
        duck_writemap(mappath, fh);

        free(fh->map);
    }
    free(fh->path);
    free(fh);

    return 0;
}

static int duck_release(const char *path, struct fuse_file_info *fi)
{
    printf("duck_release: %s\n", path);

    char *abspath = pathjoin(_srcpath, path);
    int res = duck_release_path(abspath, fi);
    free(abspath);

	return res;
}








static int xmp_fgetattr(const char *path, struct stat *stbuf,
			struct fuse_file_info *fi)
{
	printf("xmp_fgetattr\n");
    
	int res;

	(void) path;
    struct FileInfo *fh = GET_FILEINFO(fi);

	res = fstat(fh->fd, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_access(const char *path, int mask)
{
	printf("xmp_access\n");
    
	int res;

	res = access(path, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
	printf("xmp_readlink\n");
    
	int res;

	res = readlink(path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}

static int xmp_mknod(const char *path, mode_t mode, dev_t rdev)
{
	printf("xmp_mknod\n");
    
	int res;

	if (S_ISFIFO(mode))
		res = mkfifo(path, mode);
	else
		res = mknod(path, mode, rdev);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
	printf("xmp_mkdir\n");
    
	int res;

	res = mkdir(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_unlink(const char *path)
{
	printf("xmp_unlink\n");
    
	int res;

	res = unlink(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rmdir(const char *path)
{
	printf("xmp_rmdir\n");
    
	int res;

	res = rmdir(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
	printf("xmp_symlink\n");
    
	int res;

	res = symlink(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rename(const char *from, const char *to)
{
	printf("xmp_rename\n");
    
	int res;

	res = rename(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_link(const char *from, const char *to)
{
	printf("xmp_link\n");
    
	int res;

	res = link(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chmod(const char *path, mode_t mode)
{
	printf("xmp_chmod\n");
    
	int res;

	res = chmod(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid)
{
	printf("xmp_chown\n");
    
	int res;

	res = lchown(path, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_truncate(const char *path, off_t size)
{
	printf("xmp_truncate\n");
    
	int res;

	res = truncate(path, size);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_ftruncate(const char *path, off_t size,
			 struct fuse_file_info *fi)
{
	printf("xmp_ftruncate\n");
    
	int res;

	(void) path;
    struct FileInfo *fh = GET_FILEINFO(fi);

	res = ftruncate(fh->fd, size);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2])
{
	printf("xmp_utimens\n");
    
	int res;

	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}
#endif

static int xmp_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	printf("xmp_create\n");

    char *abspath = pathjoin(_srcpath, path);
    int res = duck_open_path(abspath, mode, fi);
    free(abspath);

    return res;
}

static int xmp_read_buf(const char *path, struct fuse_bufvec **bufp,
			size_t size, off_t offset, struct fuse_file_info *fi)
{
	printf("xmp_read_buf\n");
    
	struct fuse_bufvec *src;

	(void) path;
    struct FileInfo *fh = GET_FILEINFO(fi);

	src = malloc(sizeof(struct fuse_bufvec));
	if (src == NULL)
		return -ENOMEM;

	*src = FUSE_BUFVEC_INIT(size);

	src->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	src->buf[0].fd = fh->fd;
	src->buf[0].pos = offset;

	*bufp = src;

	return 0;
}

static int xmp_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	printf("xmp_write\n");

	int res;

	(void) path;
    struct FileInfo *fh = GET_FILEINFO(fi);

	res = pwrite(fh->fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	return res;
}

static int xmp_write_buf(const char *path, struct fuse_bufvec *buf,
		     off_t offset, struct fuse_file_info *fi)
{
	printf("xmp_write_buf\n");
    
	struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));

	(void) path;
    struct FileInfo *fh = GET_FILEINFO(fi);

	dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	dst.buf[0].fd = fh->fd;
	dst.buf[0].pos = offset;

	return fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
}

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
	printf("xmp_statfs\n");
    
	int res;

	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_flush(const char *path, struct fuse_file_info *fi)
{
	printf("xmp_flush\n");
    
	int res;

	(void) path;
    struct FileInfo *fh = GET_FILEINFO(fi);
	/* This is called from every close on an open file, so call the
	   close on the underlying filesystem.	But since flush may be
	   called multiple times for an open file, this must not really
	   close the file.  This is important if used on a network
	   filesystem like NFS which flush the data/metadata on close() */
	res = close(dup(fh->fd));
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	printf("xmp_fsync\n");
    
	int res;
	(void) path;
    struct FileInfo *fh = GET_FILEINFO(fi);

#ifndef HAVE_FDATASYNC
	(void) isdatasync;
#else
	if (isdatasync)
		res = fdatasync(fh->fd);
	else
#endif
		res = fsync(fh->fd);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int xmp_fallocate(const char *path, int mode,
			off_t offset, off_t length, struct fuse_file_info *fi)
{
	printf("xmp_fallocate\n");
    
	(void) path;
    struct FileInfo *fh = GET_FILEINFO(fi);

	if (mode)
		return -EOPNOTSUPP;

	return -posix_fallocate(fh->fd, offset, length);
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	printf("xmp_setxattr\n");
    
	int res = lsetxattr(path, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	printf("xmp_getxattr: %s - %s\n", path, name);
    
	int res = lgetxattr(path, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
	printf("xmp_listxattr: %s\n", path);
    
	int res = llistxattr(path, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
	printf("xmp_removexattr\n");
    
	int res = lremovexattr(path, name);
	if (res == -1)
		return -errno;
	return 0;
}
#endif /* HAVE_SETXATTR */

static int xmp_lock(const char *path, struct fuse_file_info *fi, int cmd,
		    struct flock *lock)
{
	printf("xmp_lock\n");

    return 0;
}

static int xmp_flock(const char *path, struct fuse_file_info *fi, int op)
{
	printf("xmp_flock\n");

	return 0;
}







static struct fuse_operations duck_oper = {
	.getattr	= duck_getattr,
	.readdir	= duck_readdir,
	.open		= duck_open,
	.read		= duck_read,
	.release	= duck_release,


	.fgetattr	= xmp_fgetattr,
	.access		= xmp_access,
	.readlink	= xmp_readlink,
	//.opendir	= xmp_opendir,
	//.releasedir	= xmp_releasedir,
	.mknod		= xmp_mknod,
	.mkdir		= xmp_mkdir,
	.symlink	= xmp_symlink,
	.unlink		= xmp_unlink,
	.rmdir		= xmp_rmdir,
	.rename		= xmp_rename,
	.link		= xmp_link,
	.chmod		= xmp_chmod,
	.chown		= xmp_chown,
	.truncate	= xmp_truncate,
	.ftruncate	= xmp_ftruncate,
#ifdef HAVE_UTIMENSAT
	.utimens	= xmp_utimens,
#endif
	.create		= xmp_create,
	//.read_buf	= xmp_read_buf,
	.write		= xmp_write,
	//.write_buf	= xmp_write_buf,
	.statfs		= xmp_statfs,
	//.flush		= xmp_flush,
	//.fsync		= xmp_fsync,
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= xmp_fallocate,
#endif
#ifdef HAVE_SETXATTR
	.setxattr	= xmp_setxattr,
	.getxattr	= xmp_getxattr,
	.listxattr	= xmp_listxattr,
	.removexattr	= xmp_removexattr,
#endif
	//.lock		= xmp_lock,
	//.flock		= xmp_flock,
};

static int initialize(void)
{
    return 0;
}

static void cleanup(void)
{
}

static int fuse_main_duck(int argc, char *argv[], const struct fuse_operations *op, size_t op_size, void *user_data)
{
	struct fuse *fuse;
	char *mountpoint;
	int multithreaded;
	int foreground;
	int res;

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	if (fuse_parse_cmdline(&args, &mountpoint, &multithreaded, &foreground) == -1)
		return 1;

    printf("Mount path: %s\n", mountpoint);

    struct stat st;
    if (stat(mountpoint, &st))
    {
        printf("Failed to get mount inode\n");
        return 1;
    }

    _mountino = st.st_ino;

    printf("Mount inode: %llu\n", _mountino);

	fuse = fuse_setup(argc, argv, op, op_size, &mountpoint, &multithreaded, user_data);
	if (fuse == NULL)
    {
        printf("Fuse setup failed\n");
		return 1;
    }

    int mountlen = strlen(mountpoint);
    _mountpath = malloc(mountlen + 1);
    strcpy(_mountpath, mountpoint);

    if ((_srcpath = pathup(_mountpath)))
    {
        printf("Source path: %s\n", _srcpath);

        if ((_corename = pathfile(_srcpath)))
        {
            printf("Core name: %s\n", _corename);

            if (multithreaded)
                res = fuse_loop_mt(fuse);
            else
                res = fuse_loop(fuse);
        }
        else
        {
            printf("Failed to get core name\n");
            res = -1;
        }
    }
    else
    {
        printf("Failed to get source path\n");
        res = -1;
    }

	fuse_teardown(fuse, mountpoint);
	if (res == -1)
		return 1;

	return 0;
}

int main(int argc, char *argv[])
{
    printf("Starting up...\n");

    if (initialize())
        return 1;

    umask(0);
    int err = fuse_main_duck(argc, argv, &duck_oper, sizeof(duck_oper), NULL);

    printf("\n");
    printf("Cleaning up!\n");

    cleanup();

    printf("All done!\n");

	return err ? 1 : 0;
}