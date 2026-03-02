#include "asm/asm.h"

FUNC_GLOBAL(get32)
    ldr w0, [x0]
    ret
FUNC_END(get32)

FUNC_GLOBAL(get64)
    ldr x0, [x0]
    ret
FUNC_END(get64)

FUNC_GLOBAL(put32)
    str w1, [x0]
    ret
FUNC_END(put32)

FUNC_GLOBAL(put64)
    str x1, [x0]
    ret
FUNC_END(put64)
