#ifndef __STUPIDOS_STDLIB_H__
#define __STUPIDOS_STDLIB_H__

#include_next <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include "signal.h"

extern char **environ;

void exit(int code) __attribute__((noreturn));
void _exit(int code) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*func)(void));

#endif
