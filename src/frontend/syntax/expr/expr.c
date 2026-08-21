#include "../../../chard.h"

void expr_skip_ws(expr_parser_t *ep) { while (isspace((unsigned char)*ep->p)) ep->p++; }

long expr_parse_primary(expr_parser_t *ep) {
    expr_skip_ws(ep);
    if (*ep->p == '-') {
        ep->p++;
        return -expr_parse_primary(ep);
    }
    if (*ep->p == '(') {
        ep->p++;
        long v = expr_parse_expr(ep);
        expr_skip_ws(ep);
        if (*ep->p != ')') longjmp(*ep->err, 1);
        ep->p++;
        return v;
    }
    if (!isdigit((unsigned char)*ep->p)) longjmp(*ep->err, 1);
    long v = 0;
    while (isdigit((unsigned char)*ep->p)) { v = v * 10 + (*ep->p - '0'); ep->p++; }
    return v;
}

long expr_parse_term(expr_parser_t *ep) {
    long v = expr_parse_primary(ep);
    for (;;) {
        expr_skip_ws(ep);
        if (*ep->p == '*') { ep->p++; v = v * expr_parse_primary(ep); }
        else if (*ep->p == '/') {
            ep->p++;
            long rhs = expr_parse_primary(ep);
            if (rhs == 0) longjmp(*ep->err, 1); /* division by zero in a constant expression */
            v = v / rhs;
        } else if (*ep->p == '%') {
            ep->p++;
            long rhs = expr_parse_primary(ep);
            if (rhs == 0) longjmp(*ep->err, 1);
            v = v % rhs;
        } else break;
    }
    return v;
}

long expr_parse_expr(expr_parser_t *ep) {
    long v = expr_parse_term(ep);
    for (;;) {
        expr_skip_ws(ep);
        if (*ep->p == '+') { ep->p++; v = v + expr_parse_term(ep); }
        else if (*ep->p == '-') { ep->p++; v = v - expr_parse_term(ep); }
        else break;
    }
    return v;
}

int try_parse_paren_expr(const char *tok, long *out) {
    if (tok[0] != '(') return 0;
    jmp_buf err;
    expr_parser_t ep = { tok, &err };
    if (setjmp(err)) {
        failf("malformed parenthesized expression '%s' (supported: + - * / and nested parens on plain integers)", tok);
    }
    long v = expr_parse_expr(&ep);
    expr_skip_ws(&ep);
    if (*ep.p != '\0') {
        failf("malformed parenthesized expression '%s' (supported: + - * / and nested parens on plain integers)", tok);
    }
    *out = v;
    return 1;
}

