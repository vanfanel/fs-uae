#ifndef FSEMU_CONFIG_H_
#define FSEMU_CONFIG_H_

// #ifdef HAVE_CONFIG_H
// #include "config.h"
// #endif

#define FSEMU_DEBUG 1

#ifdef FSEMU_DEBUG
//#define FSEMU_DEBUG_ALL_WARNINGS_ARE_ERRORS 1
#ifdef FSEMU_INTERNAL
#pragma GCC diagnostic error "-Wdiscarded-qualifiers"
#pragma GCC diagnostic error "-Wformat"
#pragma GCC diagnostic error "-Wincompatible-pointer-types"
#pragma GCC diagnostic error "-Wall"
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FSEMU_DEPRECATED __attribute__((deprecated))
#elif defined(_MSC_VER)
#define FSEMU_DEPRECATED __declspec(deprecated)
#else
#define FSEMU_DEPRECATED
#endif

#if 1  // def HAVE___BUILTIN_EXPECT
#define FSEMU_LIKELY(x)   __builtin_expect(!!(x), 1)
#define FSEMU_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define FSEMU_LIKELY(x)   x
#define FSEMU_UNLIKELY(x) x
#endif

#include "fsemu-common.h"

#ifdef _WIN32
#define FSEMU_WINDOWS 1
#define FSEMU_OS_WINDOWS 1
#endif

#ifdef __APPLE__
#define FSEMU_MACOS 1
#define FSEMU_OS_MACOS 1
#endif

#ifdef __linux__
#define FSEMU_LINUX 1
#define FSEMU_OS_LINUX 1
#endif

#ifdef __x86_64__
#define FSEMU_CPU_X86 1
#define FSEMU_CPU_X86_64 1
#define FSEMU_CPU_64BIT 1
#elif defined(__i386__)
#define FSEMU_CPU_X86 1
#define FSEMU_CPU_X86_32 1
#define FSEMU_CPU_32BIT 1
#endif

#ifdef __aarch64__
// #define FSEMU_ARM 1
// #define FSEMU_ARM_64 1
#define FSEMU_CPU_ARM 1
#define FSEMU_CPU_ARM_64 1
#define FSEMU_CPU_64BIT 1
#elif defined(__arm__) || defined(__ARM_EABI__)
// #define FSEMU_ARM 1
// #define FSEMU_ARM_32 1
#define FSEMU_CPU_ARM 1
#define FSEMU_CPU_ARM_32 1
#define FSEMU_CPU_32BIT 1
#endif

#if defined(FSEMU_CPU_ARM) && defined(FSEMU_OS_LINUX)
#define FSEMU_LINUX_ARM 1
#endif

#ifdef __ppc__
#define FSEMU_CPU_PPC 1
#define FSEMU_CPU_PPC_32 1
#define FSEMU_CPU_32BIT 1
#define FSEMU_CPU_BIGENDIAN 1
#endif

#define FSEMU_FLAG_NONE 0

#define FSEMU_GLIB 1
#define FSEMU_OPENGL 1
#define FSEMU_PNG 1
#define FSEMU_SDL 1
#define FSEMU_MANYMOUSE 1

#ifdef FSUAE
// FS-UAE adjusts audio frequency internally
#else
#define FSEMU_SAMPLERATE 1
#endif

/*
#ifdef FSEMU_LINUX
#ifndef FSEMU_ALSA
#define FSEMU_ALSA
#endif
#endif
*/

#define FSEMU_PATH_MAX 4096

#endif  // FSEMU_CONFIG_H_
