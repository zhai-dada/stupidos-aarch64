#ifndef __STUPIDOS_STRING_H__
#define __STUPIDOS_STRING_H__

#include_next <string.h>
#include "stupidos_user.h"

void *memcpy(void *dst, const void *src, size_t len);
void *memset(void *dst, int value, size_t len);
void *memmove(void *dst, const void *src, size_t len);
int memcmp(const void *a, const void *b, size_t len);
void *memchr(const void *s, int c, size_t n);

char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strcspn(const char *s, const char *reject);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strerror(int errnum);
char *strchrnul(const char *s, int c);
void *memrchr(const void *s, int c, size_t n);

#endif
