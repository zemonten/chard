/* sys-include.h -- auto-generated declarations for the 'include_system' module
 * (10 function(s), defined in sys-include.c). */
#ifndef CHARD_MOD_SYS_INCLUDE_H
#define CHARD_MOD_SYS_INCLUDE_H

#include "../../chard_types.h"
#include "../../chard_globals.h"

const char *intern_filename(const char *path);
int already_included(const char *resolved);
void mark_included(const char *resolved);
void include_push(const char *resolved);
void include_pop(void);
void dirname_of(const char *path, char *out, size_t outsz);
int resolve_include_path(const char *requested, const char *from_file, char *out, size_t outsz);
void parse_include_operand(const char *directive_name, const char *t, size_t kwlen, char *out, size_t outsz);
void check_include_extension(const char *directive_name, const char *resolved, int is_data);
void do_include_directive(const char *directive_name, size_t keyword_len, const char *t);

#endif /* CHARD_MOD_SYS_INCLUDE_H */
