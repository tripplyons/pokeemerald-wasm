#ifndef GUARD_WASM_STDIO_H
#define GUARD_WASM_STDIO_H

#include <stdarg.h>

int sprintf(char *str, const char *format, ...);
int vsprintf(char *str, const char *format, va_list args);
int snprintf(char *str, unsigned long size, const char *format, ...);

#endif
