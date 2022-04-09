#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "path.h"

#define BUFFER_SIZE 4096

static char *_selfexe = NULL;
static char *_selfdir = NULL;

static int pathinit(void)
{
    char buf[BUFFER_SIZE];
    int readlen;

    readlen = readlink("/proc/self/exe", buf, BUFFER_SIZE);
    if (readlen <= 0)
    {
        printf("Failed to read program path\n");
        return -1;
    }

    _selfexe = malloc(strlen(buf) + 1);
    strcpy(_selfexe, buf);

    if ((_selfdir = pathup(_selfexe)) == NULL)
    {
        printf("Failed to find program directory\n");
        return -1;
    }

    return 0;
}

char *pathselfexe(void)
{
    if (!_selfexe)
        pathinit();

    return _selfexe;
}

char *pathselfdir(void)
{
    if (!_selfdir)
        pathinit();

    return _selfdir;
}

char *pathjoin(const char *one, const char *two)
{
    if (two[0] == '/')
        two++;

    if (two[0] == '\0')
    {
        char *result = malloc(strlen(one) + 1);
        strcpy(result, one);

        return result;
    }

    char buf[BUFFER_SIZE];
    sprintf(buf, "%s/%s", one, two);

    char *result = malloc(strlen(buf) + 1);
    strcpy(result, buf);

    return result;
}

char *pathmake(const char *file)
{
    if (!_selfdir)
        pathinit();

    if (!_selfdir)
        return (char *)NULL;

    return pathjoin(_selfdir, file);
}

char *pathup(const char *path)
{
    char *lastslash;
    if ((lastslash = strrchr(path, '/')) == NULL)
    {
        printf("Failed to find parent of: %s\n", path);
        return (char *)NULL;
    }

    int len = lastslash - path;
    if (len == 0)
        return (char *)"/";

    char *result = malloc(len + 1);
    memcpy(result, path, len);
    result[len] = '\0';

    return result;
}

char *pathend(const char *path, const char *pos)
{
    int len = (path + strlen(path)) - pos - 1;
    if (len == 0)
        return (char *)"";

    char *result = malloc(len + 1);
    strcpy(result, pos + 1);

    return result;
}

char *pathfile(const char *path)
{
    char *lastslash;
    if ((lastslash = strrchr(path, '/')) == NULL)
    {
        printf("Failed to find file of: %s\n", path);
        return (char *)NULL;
    }

    return pathend(path, lastslash);
}

char *pathext(const char *path)
{
    char *lastdot;
    if ((lastdot = strrchr(path, '.')) == NULL)
    {
        printf("Failed to find extension of: %s\n", path);
        return (char *)NULL;
    }

    return pathend(path, lastdot);
}

int pathhasext(const char *path, const char *ext)
{
    if (!path || !ext)
        return 0;

    int pathlen = strlen(path);
    int extlen = strlen(ext);
    if (extlen > pathlen)
        return 0;

    for (int i = 1; i <= extlen; i++)
    {
        if (path[pathlen - i] != ext[extlen - i])
            return 0;
    }

    return 1;
}

char *strtokplus(char *s, char c, char **r)
{
    char *p = s ? s : *r;
    if (!*p)
        return 0;
    *r = strchr(p, c);
    if (*r)
        *(*r)++ = 0;
    else
        *r = p+strlen(p);
    return p;
}