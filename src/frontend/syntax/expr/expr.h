/* expr.h -- auto-generated declarations for the 'expr' module
 * (5 function(s), defined in expr.c). */
#ifndef CHARD_MOD_EXPR_H
#define CHARD_MOD_EXPR_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

void expr_skip_ws(expr_parser_t *ep);
long expr_parse_primary(expr_parser_t *ep);
long expr_parse_term(expr_parser_t *ep);
long expr_parse_expr(expr_parser_t *ep);
int try_parse_paren_expr(const char *tok, long *out);

#endif /* CHARD_MOD_EXPR_H */
