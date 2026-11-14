#ifndef TP_POCKET_STDIO_H
#define TP_POCKET_STDIO_H
#include <stdarg.h>
#include <stddef.h>
int snprintf(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
#endif
