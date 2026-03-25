#ifndef __STUPIDOS_STDIO_H__
#define __STUPIDOS_STDIO_H__

#include <stddef.h>
#include <stdarg.h>

#ifndef EOF
#define EOF (-1)
#endif

#ifndef BUFSIZ
#define BUFSIZ 1024
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

typedef struct __stupidos_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int snprintf(char *str, size_t size, const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int vsprintf(char *str, const char *fmt, va_list ap);

int puts(const char *s);
int fputs(const char *s, FILE *stream);
int fputc(int ch, FILE *stream);
int fgetc(FILE *stream);
int putchar(int ch);
int getchar(void);

FILE *fopen(const char *path, const char *mode);
FILE *fopen64(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fflush(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int ungetc(int ch, FILE *stream);
int remove(const char *pathname);

void perror(const char *s);

/*
 * 兼容高频字符 IO 宏，行为尽量贴近常见 libc。
 */
#define getc(fp)              fgetc(fp)
#define putc(ch, fp)          fputc((ch), (fp))
#define getchar()             fgetc(stdin)
#define putchar(ch)           fputc((ch), stdout)
#define getc_unlocked(fp)     fgetc(fp)
#define putc_unlocked(ch, fp) fputc((ch), (fp))

#endif
