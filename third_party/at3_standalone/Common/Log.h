#pragma once
/* ps3recomp shim for PPSSPP's logger: decoder complaints go to stderr, the
 * chatty levels are dropped. */
#include <cstdio>
#define ERROR_LOG(cat, fmt, msg) fprintf(stderr, "[cellAtrac] " fmt "%c", msg, 10)
#define WARN_LOG(cat, fmt, msg)  fprintf(stderr, "[cellAtrac] " fmt "%c", msg, 10)
#define INFO_LOG(cat, fmt, msg)  ((void)0)
#define DEBUG_LOG(cat, fmt, msg) ((void)0)
