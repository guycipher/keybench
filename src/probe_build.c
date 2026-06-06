#include <stdio.h>

#include "probe.h"

#ifndef KEYBENCH_VERSION
#define KEYBENCH_VERSION "dev"
#endif

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

extern int mallctl(const char *, void *, size_t *, void *, size_t) __attribute__((weak));
extern const char *tc_version(int *, int *, const char **) __attribute__((weak));

static const char *allocator(void)
{
    if (mallctl) return "jemalloc";
    if (tc_version) return "tcmalloc";
#ifdef __GLIBC__
    return "glibc malloc";
#else
    return "system malloc";
#endif
}

static void build_info(probe_info_cb cb, void *arg)
{
    cb(arg, "keybench", KEYBENCH_VERSION);

#ifdef __VERSION__
    cb(arg, "compiler", __VERSION__);
#elif defined(__clang_version__)
    cb(arg, "compiler", __clang_version__);
#endif

#ifdef __STDC_VERSION__
    char std[32];
#ifdef __OPTIMIZE__
    snprintf(std, sizeof std, "C %ld (optimized)", (long)__STDC_VERSION__);
#else
    snprintf(std, sizeof std, "C %ld", (long)__STDC_VERSION__);
#endif
    cb(arg, "std", std);
#endif

    char alloc[96];
#ifdef __GLIBC__
    snprintf(alloc, sizeof alloc, "%s (glibc %s)", allocator(), gnu_get_libc_version());
#else
    snprintf(alloc, sizeof alloc, "%s", allocator());
#endif
    cb(arg, "allocator", alloc);
}

static const probe BUILD_PROBE = {"build", build_info, NULL};
KV_REGISTER_PROBE(BUILD_PROBE);
