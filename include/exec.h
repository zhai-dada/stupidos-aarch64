#ifndef __EXEC_H__
#define __EXEC_H__

#include "asm/types.h"

#define EXEC_MAX_ARGS       8
#define EXEC_ARG_BUF_SIZE   256

int exec_program(const int8_t *path, int argc, const int8_t *argv[]);

#endif
