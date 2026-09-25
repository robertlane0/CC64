#include "cc64/dos64.h"

/* These definitions are compiled by CC64 for target images, never by the host. */
void cc64_exit(int code)
{
    (void)code;
    /* The startup path supplies the target exit sequence. */
}

int cc64_putc(int character)
{
    (void)character;
    return 0;
}

void *cc64_alloc(size_t size)
{
    (void)size;
    return NULL;
}

void cc64_free(void *pointer)
{
    (void)pointer;
}
