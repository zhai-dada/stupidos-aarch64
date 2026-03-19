/**
 * 保存的是字符显示像素点分布，显示标准ASCII字符。
 * 8 X 16
*/
#ifndef __FONT_H__
#define __FONT_H__

#include "asm/types.h"

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

extern uint8_t font_ascii[256][16];

#endif
