#ifndef __STUPIDOS_LIBBB_H__
#define __STUPIDOS_LIBBB_H__

/*
 * BusyBox vi 需要的最小兼容头。
 *
 * 这里不引入 BusyBox 自己那套庞大的 libbb 体系，
 * 而是把 vi 真正会用到的少量工具函数、宏和数据结构
 * 映射到 stupidos 现有的用户态兼容层上。
 */

#include "stupidos_user.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef ALIGN1
#define ALIGN1
#endif

#ifndef FAST_FUNC
#define FAST_FUNC
#endif

#ifndef MAIN_EXTERNALLY_VISIBLE
#define MAIN_EXTERNALLY_VISIBLE
#endif

#ifndef EXTERNALLY_VISIBLE
#define EXTERNALLY_VISIBLE
#endif

#ifndef ALWAYS_INLINE
#define ALWAYS_INLINE inline
#endif

#ifndef NOINLINE
#define NOINLINE
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifndef smallint
typedef int smallint;
#endif

#ifndef smalluint
typedef unsigned int smalluint;
#endif

typedef struct llist_t
{
    void *data;
    struct llist_t *link;
} llist_t;

struct globals;

#define ENABLE_LOCALE_SUPPORT 0
#define ENABLE_FEATURE_VI_8BIT 0
#define ENABLE_FEATURE_VI_COLON 1
#define ENABLE_FEATURE_VI_COLON_EXPAND 1
#define ENABLE_FEATURE_VI_YANKMARK 1
#define ENABLE_FEATURE_VI_SEARCH 1
#define ENABLE_FEATURE_VI_REGEX_SEARCH 0
#define ENABLE_FEATURE_VI_USE_SIGNALS 0
#define ENABLE_FEATURE_VI_DOT_CMD 1
#define ENABLE_FEATURE_VI_READONLY 1
#define ENABLE_FEATURE_VI_SETOPTS 1
#define ENABLE_FEATURE_VI_SET 1
#define ENABLE_FEATURE_VI_WIN_RESIZE 0
#define ENABLE_FEATURE_VI_ASK_TERMINAL 0
#define ENABLE_FEATURE_VI_UNDO 1
#define ENABLE_FEATURE_VI_UNDO_QUEUE 0
#define ENABLE_FEATURE_VI_VERBOSE_STATUS 1

#define IF_FEATURE_VI_COLON(...) __VA_ARGS__
#define IF_FEATURE_VI_COLON_EXPAND(...) __VA_ARGS__
#define IF_FEATURE_VI_YANKMARK(...) __VA_ARGS__
#define IF_FEATURE_VI_SEARCH(...) __VA_ARGS__
#define IF_FEATURE_VI_REGEX_SEARCH(...)
#define IF_FEATURE_VI_USE_SIGNALS(...)
#define IF_FEATURE_VI_DOT_CMD(...) __VA_ARGS__
#define IF_FEATURE_VI_READONLY(...) __VA_ARGS__
#define IF_FEATURE_VI_SETOPTS(...) __VA_ARGS__
#define IF_FEATURE_VI_SET(...) __VA_ARGS__
#define IF_FEATURE_VI_WIN_RESIZE(...)
#define IF_FEATURE_VI_ASK_TERMINAL(...)
#define IF_FEATURE_VI_UNDO(...) __VA_ARGS__
#define IF_FEATURE_VI_UNDO_QUEUE(...)
#define IF_FEATURE_VI_VERBOSE_STATUS(...) __VA_ARGS__

#define CONFIG_FEATURE_VI_MAX_LEN 4096
#define CONFIG_FEATURE_VI_UNDO_QUEUE_MAX 256

#define KEYCODE_BUFFER_SIZE 16
#define KEYCODE_UP          0x101
#define KEYCODE_DOWN        0x102
#define KEYCODE_LEFT        0x103
#define KEYCODE_RIGHT       0x104
#define KEYCODE_HOME        0x105
#define KEYCODE_END         0x106
#define KEYCODE_PAGEUP      0x107
#define KEYCODE_PAGEDOWN    0x108
#define KEYCODE_INSERT      0x109
#define KEYCODE_DELETE      0x10a
#define KEYCODE_FUN1        0x10b
#define KEYCODE_FUN2        0x10c
#define KEYCODE_FUN3        0x10d
#define KEYCODE_FUN4        0x10e
#define KEYCODE_FUN5        0x10f
#define KEYCODE_FUN6        0x110
#define KEYCODE_FUN7        0x111
#define KEYCODE_FUN8        0x112
#define KEYCODE_FUN9        0x113
#define KEYCODE_FUN10       0x114
#define KEYCODE_FUN11       0x115
#define KEYCODE_FUN12       0x116
#define KEYCODE_CTRL_RIGHT  0x117
#define KEYCODE_CTRL_LEFT   0x118
#define KEYCODE_ALT_RIGHT   0x119
#define KEYCODE_ALT_LEFT    0x11a
#define KEYCODE_ALT_BACKSPACE 0x11b
#define KEYCODE_ALT_D       0x11c
#define KEYCODE_CURSOR_POS  0x7ffe0001

#define TERMIOS_RAW_CRNL 1
#define BB_VER "stupidos vi"
#define STRERROR_FMT "%s"
#define STRERROR_ERRNO , strerror(errno)

extern struct globals *ptr_to_globals;
#define SET_PTR_TO_GLOBALS(x) (ptr_to_globals = (x))

extern const char bb_msg_memory_exhausted[];
extern const char bb_msg_write_error[];
extern const char bb_msg_requires_arg[];
extern const char bb_msg_invalid_arg_to[];
extern const char bb_msg_invalid_date[];
extern const char bb_msg_standard_input[];
extern const char bb_msg_standard_output[];
extern smallint bb_got_signal;

void bb_show_usage(void);
void bb_simple_error_msg_and_die(const char *s);
void bb_simple_error_msg(const char *s);
void bb_simple_perror_msg(const char *s);
void bb_simple_perror_msg_and_die(const char *s);
void bb_error_msg(const char *fmt, ...);
void bb_error_msg_and_die(const char *fmt, ...);
void bb_perror_msg(const char *fmt, ...);
void bb_perror_msg_and_die(const char *fmt, ...);
void bb_verror_msg(const char *fmt, va_list ap);
int bb_putchar(int ch);
int bb_putchar_stderr(char ch);
int fflush_all(void);
int fputs_stdout(const char *s);
void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
void *xzalloc(size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *format, ...);
unsigned bb_strtou(const char *arg, char **endp, int base);
unsigned getopt32(char **argv, const char *applet_opts, ...);
int safe_poll(struct pollfd *ufds, nfds_t nfds, int timeout);
int set_termios_to_raw(int fd, struct termios *oldterm, int flags);
int tcsetattr_stdin_TCSANOW(const struct termios *tp);
int get_terminal_width_height(int fd, unsigned *width, unsigned *height);
int get_terminal_width(int fd);
char *concat_path_file(const char *path, const char *filename);
char *skip_whitespace(const char *s);
char *skip_non_whitespace(const char *s);
char *strchrnul(const char *s, int c);
void *memrchr(const void *s, int c, size_t n);
int index_in_strings(const char *strings, const char *key);
ssize_t full_read(int fd, void *buf, size_t len);
ssize_t full_write(int fd, const void *buf, size_t len);
char *xmalloc_open_read_close(const char *filename, size_t *maxsz_p);
llist_t *llist_add_to_end(llist_t **list_head, void *data);
void *llist_pop(llist_t **head);
int64_t safe_read_key(int fd, char *buffer, int timeout);
int64_t read_key(int fd, char *buffer, int timeout);
void read_key_ungets(char *buffer, const char *str, unsigned len);
ssize_t safe_read(int fd, void *buf, size_t len);
ssize_t nonblock_immune_read(int fd, void *buf, size_t count);
void *xrealloc_vector_helper(void *vector, unsigned sizeof_and_shift, int idx);

/*
 * vi.c 里会把这些宏拼接进 usage / 选项字符串。
 * 这里保持为空实现即可，避免引入 BusyBox 原始的 config 体系。
 */

#endif
