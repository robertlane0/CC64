#ifndef CC64_TARGET_ASSERT_H
#define CC64_TARGET_ASSERT_H

/* A failed assertion has nowhere to report to in a freestanding target, so it
   stops the program. The condition is still compiled, so a side effect inside
   it is not silently dropped, and a build may define NDEBUG to remove the
   test entirely. */
#ifdef NDEBUG
#define assert(condition) ((void)0)
#else
void cc64_assert_failed(const char *expression, const char *file, int line);
#define assert(condition) \
    ((condition) ? (void)0 : cc64_assert_failed(#condition, __FILE__, __LINE__))
#endif

#endif
