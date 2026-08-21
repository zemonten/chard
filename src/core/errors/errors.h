/* errors.h -- auto-generated declarations for the 'errors' module
 * (5 function(s), defined in errors.c). */
#ifndef CHARD_MOD_ERRORS_H
#define CHARD_MOD_ERRORS_H

#include "../../chard_types.h"
#include "../../chard_globals.h"

void format_error(char *out, size_t out_sz, const char *msg);
void raise_error(const char *msg);
void fail(const char *msg);
void failf(const char *fmt, const char *arg);
void fail_fmt(const char *fmt, ...);

#endif /* CHARD_MOD_ERRORS_H */
