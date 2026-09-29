#ifndef CC64_TARGET_DIRENT_H
#define CC64_TARGET_DIRENT_H

#include <stddef.h>

/* The target volume holds one flat directory: it has no subdirectories, so a
   read returns every entry and the end of it. The entry is the target's own
   record, so a caller reads the name through the same field it declares. */

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

#define NAME_MAX 255

struct dirent {
    unsigned long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[NAME_MAX + 1];
};

typedef struct cc64_dir DIR;

/* A name that is neither "." nor ".." and does not start with a dot, because
   the target's directory has no such entries. The reader is the only way to
   see one, and it returns nothing once every entry has been read. */
struct dirent *readdir(DIR *stream);
int closedir(DIR *stream);
DIR *opendir(const char *path);
int dirfd(DIR *stream);
void rewinddir(DIR *stream);

#endif
