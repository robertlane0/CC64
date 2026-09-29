#ifndef CC64_TARGET_DLFCN_H
#define CC64_TARGET_DLFCN_H

/* A target image is one program with no dynamic loader, so a shared object
   cannot be opened. The names are present so a program that links against
   them compiles, and each reports that there is nothing to open rather than
   failing to link, because a program that treats the library as optional
   should still run. */

#define RTLD_LAZY 1
#define RTLD_NOW  2
#define RTLD_GLOBAL 0x100

void *dlopen(const char *name, int flags);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);

#endif
