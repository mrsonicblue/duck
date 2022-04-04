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

static void duck_parsepathrelease(struct PathInfo *info)
{
    int i;
    for (i = info->stacklen - 1; i >= 0; i--)
    {
        free(info->stack[i]);
    }

    if (info->filepath)
        free(info->filepath);
}

static int duck_isfile(struct PathInfo *info)
{
    printf("stacklen: %d\n", info->stacklen);

    if (info->stacklen > 0)
        return 1;

    return 0;
}

static char *duck_filepath(struct PathInfo *info)
{
    char buf[BUFFER_SIZE];
    sprintf(buf, "%s/%s", _srcpath, info->stack[info->stacklen - 1]);

    char *result = malloc(strlen(buf) + 1);
    strcpy(result, buf);

    return result;
}

static int duck_parsepath(struct PathInfo *info, const char *path)
{
    memset(info, 0, sizeof(struct PathInfo));

    char pathdup[BUFFER_SIZE];
    strcpy(pathdup, path);

    // Skip leading slash in path
    char *pathbeg = pathdup;
    if (*pathbeg == '/')
        pathbeg++;

    char *r = NULL;
    char *t;
    char *tmp;
    int pos = 0;
    for (t = strtokplus(pathbeg, '/', &r); t != NULL; t = strtokplus(NULL, '/', &r))
    {
        tmp = malloc(strlen(t) + 1);
        strcpy(tmp, t);
        
        info->stack[pos++] = tmp;
    }
    info->stacklen = pos;

    if ((info->isfile = duck_isfile(info)))
        info->filepath = duck_filepath(info);

    return 0;
}

static int duck_getattr_fakedir(struct PathInfo *info, struct stat *stbuf)
{
    (void) info;

    memset(stbuf, 0, sizeof(struct stat));
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 1;

    return 0;
}

static int duck_getattr_file(struct PathInfo *info, struct stat *stbuf)
{
    int res;
	if ((res = lstat(info->filepath, stbuf)) == -1)
		return -errno;

    return 0;
}

static int duck_getattr(const char *path, struct stat *stbuf)
{
    printf("duck_getattr: %s\n", path);

    struct PathInfo info;
    if (duck_parsepath(&info, path))
        return -ENOENT;

    int res;
    if (info.isfile)
        res = duck_getattr_file(&info, stbuf);
    else
        res = duck_getattr_fakedir(&info, stbuf);

    duck_parsepathrelease(&info);

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

static void duck_readdir_root(struct PathInfo *info, void *buf, fuse_fill_dir_t filler)
{
    (void) info;

    DIR *dp;
	if ((dp = opendir(_srcpath)) == NULL)
		return;
    
	struct dirent *de;
	while ((de = readdir(dp)) != NULL)
    {
        if (de->d_type == 8 /* DT_REG */)
        {
            struct stat st;
            memset(&st, 0, sizeof(st));
            st.st_ino = de->d_ino;
            st.st_mode = de->d_type << 12;

            if (filler(buf, de->d_name, &st, 0))
                break;
        }
	}

    closedir(dp);
}

static int duck_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    //printf("duck_readdir: %s\n", path);

	(void) offset;
	(void) fi;

    struct PathInfo info;
    if (duck_parsepath(&info, path))
        return -ENOENT;

    duck_fakefill(buf, ".", filler);
    duck_fakefill(buf, "..", filler);

    duck_readdir_root(&info, buf, filler);

    duck_parsepathrelease(&info);

    return 0;
}

static int duck_open(const char *path, struct fuse_file_info *fi)
{
    printf("duck_open: %s\n", path);

    struct PathInfo info;
    if (duck_parsepath(&info, path))
        return -ENOENT;

    if (!info.isfile)
        return -ENOENT;

    int fd;
    if ((fd = open(info.filepath, fi->flags)) == -1)
        return -errno;

    fi->fh = fd;

    return 0;
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