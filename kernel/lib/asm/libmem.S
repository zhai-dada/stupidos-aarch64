#include "asm/asm.h"

/*
    x0, address
    x1, data
    x2, count
 */
FUNC_GLOBAL(__memset_16bytes)
    mov x4, 0
1:  stp x1, x1, [x0], #16
    add x4, x4, #1
    cmp x4, x2
    bne 1b
    ret
FUNC_END(__memset_16bytes)

/*
    x0, address
    x1, data
    x2, count
 */
FUNC_GLOBAL(__memset_1bytes)
    mov x4, 0
1:  strb w1, [x0], #1
    add x4, x4, #1
    cmp x4, x2
    bne 1b
    ret
FUNC_END(__memset_1bytes)
