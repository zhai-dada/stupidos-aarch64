#ifndef __PRINTK_H__
#define __PRINTK_H__

#include "asm/types.h"
#include "stdarg.h"
#include "lib/libstr.h"

/**
 * 格式化显示，格式符
 * 补0，符号，加号，空格，左对其，#，小写
*/
#define ZEROPAD 0b0000001   /*0*/
#define SIGN    0b0000010
#define PLUS    0b0000100   /*+*/
#define SPACE   0b0001000   /* */
#define LEFT    0b0010000   /*-*/
#define SPECIAL 0b0100000   /*0x*/
#define SMALL   0b1000000

int32_t vsprintf(int8_t *buf, const int8_t *fmt, va_list args);
int32_t sprintf(int8_t *buffer, const int8_t *fmt, ...);

#endif
