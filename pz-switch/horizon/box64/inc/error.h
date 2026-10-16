#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
static inline void error(int status, int errnum, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); if (errnum) fprintf(stderr, ": %s", strerror(errnum)); fputc('\n', stderr); if (status) exit(status); }
static inline void error_at_line(int status, int errnum, const char *file, unsigned line, const char *fmt, ...)
{ va_list ap; fprintf(stderr, "%s:%u: ", file, line); va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); if (errnum) fprintf(stderr, ": %s", strerror(errnum)); fputc('\n', stderr); if (status) exit(status); }
