/* cond.h -- auto-generated declarations for the 'scopes_cond' module
 * (7 function(s), defined in cond.c). */
#ifndef CHARD_MOD_COND_H
#define CHARD_MOD_COND_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

void push__scope(scope_t s);
void finalize_pending_if_scope(void);
scope_t *find_enclosing_loop_scope(void);
opcode_t invert_cond_op(const char *op, int is_unsigned);
int is_cond_op(const char *tok);
opcode_t parse_cond_and_emit_cmp_ex(const char *raw_after_keyword, int require_brace);
opcode_t parse_cond_and_emit_cmp(const char *raw_after_keyword);

#endif /* CHARD_MOD_COND_H */
