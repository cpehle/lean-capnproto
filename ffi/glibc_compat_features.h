#ifndef CAPNP_LEAN_GLIBC_COMPAT_FEATURES_H
#define CAPNP_LEAN_GLIBC_COMPAT_FEATURES_H

#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <features.h>
#if defined(__GLIBC__)
#undef __GLIBC_USE_C2X_STRTOL
#define __GLIBC_USE_C2X_STRTOL 0
#endif
#endif

#endif
