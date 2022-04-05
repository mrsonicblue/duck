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

struct PathInfo
{
    char *stack[20];
    int stacklen;
    int isfile;
    char *filepath;
};

static char *_mountpath;
static char *_srcpath;
static char *_corename;

static int duck_getattr_fakedir(struct PathInfo *info, struct stat *stbuf)
{
    (void) info;

    memset(stbuf, 0, sizeof(struct stat));
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 1;

    return 0;
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

static void duck_fakefill(void *buf, const char *name, fuse_fill_dir_t filler)
{
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFDIR;

    filler(buf, name, &st, 0);
}

static void duck_readdir_alpha_letter(struct PathInfo *info, void *buf, fuse_fill_dir_t filler)
{
    // (void) info;

	// DIR *dp;
	// if ((dp = opendir(_srcpath)) == NULL)
	// 	return;

    // char letter1 = info->stack[1][0];
    // char letter2;
    // if (letter1 >= 'A' && letter1 <= 'Z')
    // {
    //     letter2 = letter1 + ('a' - 'A');
    // }
    // else
    // {
    //     letter1 = '0';
    //     letter2 = '0';
    // }

	// struct dirent *de;
	// while ((de = readdir(dp)) != NULL)
    // {
    //     if (de->d_type == 8 /* DT_REG */)
    //     {
    //         char first = de->d_name[0];
    //         if (letter1 == '0')
    //         {
    //             if ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z'))
    //                 continue;
    //         }
    //         else
    //         {
    //             if (first != letter1 && first != letter2)
    //                 continue;
    //         }

    //         struct stat st;
    //         memset(&st, 0, sizeof(st));
    //         st.st_ino = de->d_ino;
    //         st.st_mode = de->d_type << 12;

    //         if (filler(buf, de->d_name, &st, 0))
    //             break;
    //     }
	// }

	// closedir(dp);
}

static void duck_readdir_path(const char *path, void *buf, fuse_fill_dir_t filler)
{
    DIR *dp;
	if ((dp = opendir(path)) == NULL)
		return;
    
	struct dirent *de;
	while ((de = readdir(dp)) != NULL)
    {
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

static int duck_open_path(const char *path, struct fuse_file_info *fi)
{
    int fd;
    if ((fd = open(path, fi->flags)) == -1)
        return -errno;

    fi->fh = fd;

    return 0;
}

static int duck_open(const char *path, struct fuse_file_info *fi)
{
    printf("duck_open: %s\n", path);

    char *abspath = pathjoin(_srcpath, path);
    int res = duck_open_path(abspath, fi);
    free(abspath);

    return res;
}

static int duck_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    printf("duck_read: %s\n", path);

    (void) path;

    int res;
    if ((res = pread(fi->fh, buf, size, offset)) == -1)
        res = -errno;

    return res;
}

static int duck_release(const char *path, struct fuse_file_info *fi)
{
    printf("duck_release: %s\n", path);

    (void) path;
    close(fi->fh);

	return 0;
}

static struct fuse_operations duck_oper = {
	.getattr	= duck_getattr,
	.readdir	= duck_readdir,
	.open		= duck_open,
	.read		= duck_read,
	.release	= duck_release
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
	int res;

	fuse = fuse_setup(argc, argv, op, op_size, &mountpoint, &multithreaded, user_data);
	if (fuse == NULL)
    {
        printf("Fuse setup failed\n");
		return 1;
    }

    printf("Mount path: %s\n", mountpoint);

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