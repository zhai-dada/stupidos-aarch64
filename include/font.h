/*
 * 保存的是字符显示像素点分布，显示标准 ASCII 字符。
 * 构建时会基于 Monaco.ttf 生成 8x16 点阵。
 */
#ifndef __FONT_H__
#define __FONT_H__

#include "asm/types.h"

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

extern uint8_t font_ascii[256][16];

#endif
