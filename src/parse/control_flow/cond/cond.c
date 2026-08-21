#include "../../../chard.h"

void push__scope(scope_t s) {
    if (scope_depth >= MAX_SCOPE_DEPTH) fail("if/while/for nesting too deep");
    scope_stack[scope_depth++] = s;
}

void finalize_pending_if_scope(void) {
    if (scope_depth == 0) return;
    scope_t *s = &scope_stack[scope_depth - 1];
    if (s->kind != SCOPE_IF || !s->pending_close) return;

    if (!s->has_else) {
        instr_t else_lbl; memset(&else_lbl, 0, sizeof(else_lbl));
        else_lbl.op = OP_LABEL;
        strncpy(else_lbl.dst.sym, s->else_label, MAX_SYMLEN - 1);
        push__instr(else_lbl);
    }
    instr_t end; memset(&end, 0, sizeof(end));
    end.op = OP_LABEL;
    strncpy(end.dst.sym, s->end_label, MAX_SYMLEN - 1);
    push__instr(end);
    scope_depth--;
}

scope_t *find_enclosing_loop_scope(void) {
    for (int i = scope_depth - 1; i >= 0; i--) {
        if (scope_stack[i].kind == SCOPE_WHILE || scope_stack[i].kind == SCOPE_FOR)
            return &scope_stack[i];
    }
    return NULL;
}

opcode_t invert_cond_op(const char *op, int is_unsigned) {
    if (strcmp(op, "==") == 0) return OP_JNE;
    if (strcmp(op, "!=") == 0) return OP_JE;
    if (strcmp(op, ">") == 0)  return is_unsigned ? OP_JBE : OP_JLE;
    if (strcmp(op, "<") == 0)  return is_unsigned ? OP_JAE : OP_JGE;
    if (strcmp(op, ">=") == 0) return is_unsigned ? OP_JB  : OP_JL;
    if (strcmp(op, "<=") == 0) return is_unsigned ? OP_JA  : OP_JG;
    return OP_JMP; /* unreachable: caller validates op first */
}

int is_cond_op(const char *tok) {
    return strcmp(tok, "==") == 0 || strcmp(tok, "!=") == 0 ||
           strcmp(tok, ">") == 0 || strcmp(tok, "<") == 0 ||
           strcmp(tok, ">=") == 0 || strcmp(tok, "<=") == 0;
}

opcode_t parse_cond_and_emit_cmp_ex(const char *raw_after_keyword, int require_brace) {
    char buf[MAX_LINE];
    strncpy(buf, raw_after_keyword, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *term = strrchr(buf, require_brace ? '{' : ';');
    if (!term) fail(require_brace ? "expected '{' to open if/while body"
                                   : "expected ';' to end 'assert' statement");
    *term = '\0';

    char *toks[MAX_TOKENS];
    int n = tokenize(buf, toks);
    if (n < 3) fail("malformed condition: expected 'LHS OP RHS {'");

    int is_unsigned = 0;
    const char *optok = toks[1];
    if (optok[0] == 'u' && is_cond_op(optok + 1)) {
        is_unsigned = 1;
        optok++;
    }
    if (!is_cond_op(optok)) failf("unknown comparison operator '%s'", optok);

    /* everything from toks[2] onward is the RHS in case it was split
       (shouldn't normally happen since operands don't contain spaces,
       but a stray extra token is a clearer error than silent misparse) */
    if (n != 3) fail("malformed condition: expected exactly 'LHS OP RHS {'");

    instr_t cmp; memset(&cmp, 0, sizeof(cmp));
    cmp.op = OP_CMP;
    /* OP_CMP follows the same 'src > dst' convention as add/sub/etc:
       dst is the left/accumulator operand, src is the right operand,
       so 'cmp dst, src' in codegen reads as "compare LHS against RHS"
       exactly as written in the if/while condition. */
    parse__operand(toks[0], &cmp.dst);  /* LHS */
    parse__operand(toks[2], &cmp.src);  /* RHS */
    if (cmp.dst.kind != OPND_REG)
        fail("condition's left-hand side must be a register (LHS OP RHS)");
    push__instr(cmp);

    return invert_cond_op(optok, is_unsigned);
}

opcode_t parse_cond_and_emit_cmp(const char *raw_after_keyword) {
    return parse_cond_and_emit_cmp_ex(raw_after_keyword, 1);
}

