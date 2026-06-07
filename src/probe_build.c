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

    /* Name the compiler, not just its version. GCC's __VERSION__ is the bare
       number with no name, and clang defines __GNUC__ too, so check clang first. */
    char comp[64];
#if defined(__clang__)
    snprintf(comp, sizeof comp, "clang %d.%d.%d", __clang_major__, __clang_minor__,
             __clang_patchlevel__);
#elif defined(__GNUC__)
    snprintf(comp, sizeof comp, "gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    snprintf(comp, sizeof comp, "unknown");
#endif
    cb(arg, "compiler", comp);

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
