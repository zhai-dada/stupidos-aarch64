#ifndef STUPIDOS_BUSYBOX_UNICODE_H
#define STUPIDOS_BUSYBOX_UNICODE_H

/*
 * BusyBox lineedit 的最小 unicode 兼容层。
 * 当前系统先按 ASCII / UTF-8 兼容的简化模式运行，
 * 不引入 upstream 那套完整 unicode 实现，减少接入复杂度。
 */

#include <string.h>

enum
{
    UNICODE_UNKNOWN = 0,
    UNICODE_OFF = 1,
    UNICODE_ON = 2,
};

#define unicode_bidi_isrtl(wc) 0
#define unicode_bidi_is_neutral_wchar(wc) ((wc) <= 126 && !isalpha((unsigned char)(wc)))
#define unicode_strlen(string) strlen(string)
#define unicode_strwidth(string) strlen(string)
#define unicode_status UNICODE_OFF
#define init_unicode() ((void)0)
#define reinit_unicode(LANG) ((void)0)

#endif
