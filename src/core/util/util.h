/* util.h -- auto-generated declarations for the 'util' module
 * (17 function(s), defined in util.c). */
#ifndef CHARD_MOD_UTIL_H
#define CHARD_MOD_UTIL_H

#include "../../chard_types.h"
#include "../../chard_globals.h"

int in_string_at(const char *line_start, const char *p);
const char *find_string_close_quote(const char *q1);
char *strip__comment(char *line);
char *trim(char *s);
char *str_dup(const char *s);
int is_ident_char(char c);
size_t buf__append(char *dst, size_t cap, size_t len, const char *src, size_t srclen);
int is__decimal_number(const char *tok);
int is__hex_number(const char *tok);
int is__binary_number(const char *tok);
int is__number(const char *tok);
int is_float_literal(const char *tok);
void strip__semicolon(char *tok);
void strip__trailing_comma(char *tok);
mem_order_t parse_mem_order_suffix(const char *raw_trimmed, const char *opname);
int has_extension(const char *path, const char *ext);
int stack_slot_size(target_t t);

#endif /* CHARD_MOD_UTIL_H */
