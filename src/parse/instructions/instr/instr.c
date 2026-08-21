#include "../../../chard.h"

/* Lookup tables used only within this module. */
static opmap_t two_operand_ops[] = {
    {"mv", OP_MOV}, {"load", OP_LOAD}, {"store", OP_STORE}, {"lea", OP_LEA},
    {"add", OP_ADD}, {"sub", OP_SUB}, {"mul", OP_MUL}, {"div", OP_DIV}, {"mod", OP_MOD},
    {"and", OP_AND}, {"or", OP_OR}, {"xor", OP_XOR},
    {"shl", OP_SHL}, {"shr", OP_SHR}, {"cmp", OP_CMP},
    {"not", OP_NOT}, {"neg", OP_NEG},
    {"rotl", OP_ROTL}, {"rotr", OP_ROTR},
    {"popcount", OP_POPCOUNT}, {"clz", OP_CLZ}, {"ctz", OP_CTZ},
    {"sat_add", OP_SAT_ADD}, {"sat_sub", OP_SAT_SUB},
    /* Symbolic aliases -- same opcode as the word form (e.g. '& r1,
       r2;' is exactly 'and r1, r2;'), just spelled with the C-style
       operator instead of the mnemonic, for people used to writing
       '+'/'<<'/'&'/etc out of habit. These are ordinary extra entries
       in this table, not a separate grammar: they go through the
       exact same dispatch loops (matched on tokens[0], same operand
       parsing, same destination-must-be-a-register check), so
       anything already true of 'add'/'and'/'or'/etc is automatically
       true of their symbol equivalents too -- INCLUDING the '>'
       spelling: the word forms accept both 'op src > dst;' and
       'op src, dst;', and so do the symbolic aliases now -- '>' isn't
       withheld from the symbols, both delimiters mean the exact same
       thing for them as for the word mnemonics. See is_symbolic_op_name
       and the 'op src > dst' parsing site below (this table alone
       doesn't distinguish '+' from 'add' by opcode, since both share
       OP_ADD -- the distinction is only ever about which spelling was
       typed, and both spellings are accepted for both).
       '+'/'-'/'*'/'/'/'%' ARE included here (unlike an earlier version
       of this comment, which excluded them over a concern that turned
       out not to apply): the worry was that a leading '-' or '+' reads
       as part of a signed immediate -- true INSIDE a token ('-5' is one
       token, parsed as negative five), but never true of tokens[0]
       itself, since tokenize() only splits on whitespace: a source line
       starting '+ r1, r5;' or '- r1, r5;' always produces '+' or '-' as
       their own standalone first token, distinct from an immediate like
       '-5' or '+5' (which would never appear alone in that position --
       an immediate is always an operand, never tokens[0]). So there's
       no actual ambiguity to guard against there. Must stay in sync
       with symbolic_ops[]
       (fused-store's EXPR operators) below -- both tables map the same
       symbols to the same opcodes, just for different call sites
       ('op src > dst;' / 'op src, dst;' here vs.
       'DST(*rX) := rX(a OP b);' there). No symbol for 'cmp' (no natural
       single-glyph analog) or for 'neg' (already '~' would be
       ambiguous with bitwise NOT's '~', and 'neg' is comparatively rare
       next to '-' meaning subtract, which takes the obvious symbol
       instead). */
    {"+", OP_ADD}, {"-", OP_SUB}, {"*", OP_MUL}, {"/", OP_DIV}, {"%", OP_MOD},
    {"&", OP_AND}, {"|", OP_OR}, {"^", OP_XOR},
    {"<<", OP_SHL}, {">>", OP_SHR}, {"~", OP_NOT},
};
#define N_TWO_OP (sizeof(two_operand_ops)/sizeof(two_operand_ops[0]))
static opmap_t float_ops[] = {
    {"fmov", OP_FMOV}, {"fload", OP_FLOAD}, {"fstore", OP_FSTORE},
    {"fadd", OP_FADD}, {"fsub", OP_FSUB}, {"fmul", OP_FMUL}, {"fdiv", OP_FDIV},
    {"fcmp", OP_FCMP}, {"i2f", OP_I2F}, {"f2i", OP_F2I},
    {"fsqrt", OP_FSQRT}, {"fabs", OP_FABS}, {"fneg", OP_FNEG}, {"fmin", OP_FMIN}, {"fmax", OP_FMAX},
    {"vadd", OP_VADD}, {"vsub", OP_VSUB}, {"vmul", OP_VMUL}, {"vdiv", OP_VDIV},
    {"vmin", OP_VMIN}, {"vmax", OP_VMAX},
    {"vsqrt", OP_VSQRT}, {"vabs", OP_VABS}, {"vneg", OP_VNEG}, {"vdup", OP_VDUP},
    {"vload", OP_VLOAD}, {"vstore", OP_VSTORE},
};
#define N_FLOAT_OP (sizeof(float_ops)/sizeof(float_ops[0]))
static opmap_t symbolic_ops[] = {
    {"+", OP_ADD}, {"-", OP_SUB}, {"*", OP_MUL}, {"/", OP_DIV}, {"%", OP_MOD},
    {"&", OP_AND}, {"|", OP_OR}, {"^", OP_XOR}, {"<<", OP_SHL}, {">>", OP_SHR},
};
#define N_SYM_OP (sizeof(symbolic_ops)/sizeof(symbolic_ops[0]))
static opmap_t jump_ops[] = {
    {"jmp", OP_JMP}, {"je", OP_JE}, {"jne", OP_JNE},
    {"jz", OP_JE}, {"jnz", OP_JNE},  /* pure mnemonic aliases for je/jne
        -- map to the exact same opcodes, not new ones, so every switch
        over opcode_t elsewhere in this file (backend codegen, the
        pin-violation checker, etc) already handles them with zero
        further changes. 'zero'/'not-zero' is the customary way to
        spell an equality test after a flags-setting op on x86 (jz/jnz
        are themselves already just assembler aliases for je/jne at the
        machine-code level -- same opcode byte), so Chard's own je/jne
        keep their existing names as the primary spelling and jz/jnz
        are offered as the familiar alternative some callers reach for
        out of x86 habit, rather than as an independent third condition
        AArch64/RISC-V codegen would need to know how to lower. */
    {"jg", OP_JG}, {"jl", OP_JL}, {"jge", OP_JGE}, {"jle", OP_JLE},
    {"ja", OP_JA}, {"jb", OP_JB}, {"jae", OP_JAE}, {"jbe", OP_JBE},
    {"call", OP_CALL},
};
#define N_JUMP_OP (sizeof(jump_ops)/sizeof(jump_ops[0]))

int parse_fused_store(const char *raw_trimmed) {
    const char *paren1 = strchr(raw_trimmed, '(');
    if (!paren1) return 0;
    const char *star = paren1 + 1;
    while (isspace((unsigned char)*star)) star++;
    if (*star != '*') return 0; /* not this form: no '(*' after the name */

    char line[MAX_LINE];
    strncpy(line, raw_trimmed, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strip__semicolon(line);

    /* DST( * rDST_REG ) := rEXPR_REG ( EXPR ) */
    char dst_name[MAX_SYMLEN], dst_reg[MAX_SYMLEN];
    char expr_reg[MAX_SYMLEN], expr_body[MAX_LINE];
    int consumed = 0;

    if (sscanf(line, " %63[^ (] ( * %63[^ )] ) %n", dst_name, dst_reg, &consumed) != 2)
        fail("malformed fused store: expected 'DST(*rX) := rX(EXPR);'");

    const char *rest = line + consumed;
    rest += strspn(rest, " \t");
    if (strncmp(rest, ":=", 2) != 0)
        fail("malformed fused store: expected ':=' after 'DST(*rX)'");
    rest += 2;
    rest += strspn(rest, " \t");

    const char *ep1 = strchr(rest, '(');
    const char *ep2 = strrchr(rest, ')');
    if (!ep1 || !ep2 || ep2 < ep1)
        fail("malformed fused store: expected 'rX(EXPR)' after ':='");
    size_t reglen = (size_t)(ep1 - rest);
    if (reglen == 0 || reglen >= sizeof(expr_reg))
        fail("malformed fused store: bad register before '(' on right-hand side");
    memcpy(expr_reg, rest, reglen);
    expr_reg[reglen] = '\0';
    char *rtrim = trim(expr_reg);

    size_t bodylen = (size_t)(ep2 - ep1 - 1);
    if (bodylen >= sizeof(expr_body)) fail("expression too long in fused store");
    memcpy(expr_body, ep1 + 1, bodylen);
    expr_body[bodylen] = '\0';
    char *body = trim(expr_body);

    if (!find__decl(dst_name)) failf("fused store: unknown destination symbol '%s'", dst_name);

    operand_t dstregop, exprregop;
    if (!parse__register(dst_reg, &dstregop)) failf("fused store: '%s' is not a valid register", dst_reg);
    if (!parse__register(rtrim, &exprregop)) failf("fused store: '%s' is not a valid register", rtrim);
    if (dstregop.reg_num != exprregop.reg_num || dstregop.is_sp != exprregop.is_sp)
        fail("fused store: the register in '(*rX)' and 'rX(...)' must be the same");

    /* Split EXPR on the first recognized symbolic operator, checked
     * longest-first so '<<'/'>>' aren't mis-split as two '<'/'>' or
     * confused with a bare '-' inside a negative-immediate operand. */
    const char *found_op = NULL;
    opcode_t found_opcode = OP_MOV;
    for (size_t k = 0; k < N_SYM_OP; k++) {
        const char *hit = strstr(body, symbolic_ops[k].name);
        /* skip a leading '-' that's actually part of a negative number,
           e.g. body == "-5" with no left-hand operand */
        if (hit == body && symbolic_ops[k].name[0] == '-') continue;
        if (hit && (!found_op || hit < found_op)) {
            found_op = hit;
            found_opcode = symbolic_ops[k].op;
        }
    }

    instr_t compute; memset(&compute, 0, sizeof(compute));
    if (found_op) {
        char lhs[MAX_SYMLEN];
        size_t oplen = 1;
        for (size_t k = 0; k < N_SYM_OP; k++) {
            if (symbolic_ops[k].op == found_opcode &&
                strncmp(found_op, symbolic_ops[k].name, strlen(symbolic_ops[k].name)) == 0) {
                oplen = strlen(symbolic_ops[k].name);
                break;
            }
        }
        size_t lhslen = (size_t)(found_op - body);
        if (lhslen >= sizeof(lhs)) fail("left-hand operand too long in fused store expression");
        memcpy(lhs, body, lhslen);
        lhs[lhslen] = '\0';
        char *lhs_t = trim(lhs);
        char *rhs_t = trim((char *)found_op + oplen);
        if (*lhs_t == '\0' || *rhs_t == '\0')
            fail("malformed expression in fused store: missing operand around operator");

        /* Seed rX with the left operand, then apply the operator with
           the right operand as src (codegen for binops treats dst as
           the running accumulator). A symbol operand must become an
           OP_LOAD (mov has no defined "load this memory symbol" form
           anywhere else in the compiler); reg/imm use OP_MOV as usual. */
        instr_t seed; memset(&seed, 0, sizeof(seed));
        parse__operand(lhs_t, &seed.src);
        seed.op = is_mem_operand(seed.src.kind) ? OP_LOAD : OP_MOV;
        seed.dst = dstregop;
        push__instr(seed);

        compute.op = found_opcode;
        parse__operand(rhs_t, &compute.src);
        compute.dst = dstregop;
        push__instr(compute);
    } else {
        if (*body == '\0') fail("empty expression in fused store");
        parse__operand(body, &compute.src);
        compute.op = is_mem_operand(compute.src.kind) ? OP_LOAD : OP_MOV;
        compute.dst = dstregop;
        push__instr(compute);
    }

    instr_t store; memset(&store, 0, sizeof(store));
    store.op = OP_STORE;
    store.src = dstregop;
    parse__operand(dst_name, &store.dst); /* resolves to OPND_LOCAL if dst_name
                                             names an in-scope local, same as
                                             every other operand in the file */
    push__instr(store);

    return 1;
}

int syscall_num_for_name(const char *tok, long *out) {
    for (int i = 0; i < NUM_SYSCALL_NAMES; i++) {
        if (strcmp(syscall_names[i].name, tok) != 0) continue;
        long n = (g_target == TARGET_X86_64) ? syscall_names[i].x86_64
                                              : syscall_names[i].generic;
        if (n < 0) return -1; /* explicitly unavailable on this target */
        *out = n;
        return 1;
    }
    return 0;
}

void emit_array_bounds_check(operand_t idx_reg, int len) {
    instr_t cmp; memset(&cmp, 0, sizeof(cmp));
    cmp.op = OP_CMP;
    cmp.dst = idx_reg; /* OP_CMP requires a register destination --
                           already guaranteed here, since this is
                           only ever called with an already-
                           validated register operand */
    cmp.src.kind = OPND_IMM;
    cmp.src.imm = len;
    push__instr(cmp);

    instr_t a; memset(&a, 0, sizeof(a));
    a.op = OP_ASSERT;
    a.assert_jmp_op = OP_JAE; /* traps when idx >= len, unsigned --
                                  see this function's own comment */
    push__instr(a);
}

void parse_libc_call_body(const char *name_start, const char *kw_for_errors) {
    const char *paren_start = strchr(name_start, '(');
    if (!paren_start) failf("malformed %s: missing '('", kw_for_errors);
    size_t namelen = (size_t)(paren_start - name_start);
    char fname[MAX_SYMLEN];
    if (namelen == 0 || namelen >= MAX_SYMLEN) failf("malformed %s: missing function name", kw_for_errors);
    memcpy(fname, name_start, namelen);
    fname[namelen] = '\0';

    extern_sig_t *sig = find_extern(fname);
    if (!sig) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s to undeclared function '%s' (libc functions must be declared with 'extern %s(nargs);' before they're called)", kw_for_errors, fname, fname);
        fail(msg);
    }

    const char *close_paren = strchr(paren_start, ')');
    if (!close_paren) failf("malformed %s: missing ')'", kw_for_errors);
    char buf[MAX_LINE];
    size_t arglen = (size_t)(close_paren - paren_start - 1);
    if (arglen >= sizeof(buf)) arglen = sizeof(buf) - 1;
    strncpy(buf, paren_start + 1, arglen);
    buf[arglen] = '\0';

    instr_t c; memset(&c, 0, sizeof(c));
    c.op = OP_LIBC_CALL;
    strncpy(c.dst.sym, fname, MAX_SYMLEN - 1); /* dst.sym doubles as the
        callee name here (dst.kind stays OPND_NONE unless '> rX'
        below sets it) since OP_LIBC_CALL has no other field that
        naturally carries "which external function" the way
        OP_CALL's dst (a label operand) already does */

    char *tok = strtok(buf, ",");
    int nargs = 0;
    while (tok) {
        char *a = trim(tok);
        if (*a != '\0') {
            if (nargs >= MAX_LIBC_ARGS) {
                char msg[128];
                snprintf(msg, sizeof(msg), "too many %s arguments (max %d, matching the target's register-argument ABI)", kw_for_errors, MAX_LIBC_ARGS);
                fail(msg);
            }
            parse__operand(a, &c.args[nargs]);
            nargs++;
        }
        tok = strtok(NULL, ",");
    }
    if (nargs != sig->nargs) {
        char msg[256];
        snprintf(msg, sizeof(msg), "wrong number of arguments in %s to '%s' (see its matching 'extern %s(%d);' declaration)", kw_for_errors, fname, fname, sig->nargs);
        fail(msg);
    }
    c.nargs = nargs;
    push__instr(c);

    /* Optional '> rX' captures the return value out of the target's
       native return register (rax/x0/a0) into a Chard r1-r12 register --
       same shape as call()'s '> rX', just always moving from the one
       fixed ABI return register instead of a per-function declared
       one. */
    const char *arrow_gt = strchr(close_paren, '>');
    if (arrow_gt) {
        char destbuf[32];
        const char *ds = arrow_gt + 1;
        while (*ds == ' ') ds++;
        const char *de = strchr(ds, ';');
        if (!de) failf("malformed %s: missing ';'", kw_for_errors);
        size_t dl = (size_t)(de - ds);
        if (dl == 0 || dl >= sizeof(destbuf)) failf("malformed %s: expected a register after '>'", kw_for_errors);
        memcpy(destbuf, ds, dl);
        destbuf[dl] = '\0';
        operand_t destop;
        parse__operand(destbuf, &destop);
        if (destop.kind != OPND_REG || destop.is_sp) failf("%s result destination must be a register (e.g. > r6), not sp or anything else", kw_for_errors);
        instr_t *libc_ins = &prog[nprog - 1];
        libc_ins->dst.kind = OPND_REG;
        libc_ins->dst.reg_num = destop.reg_num;
        /* NOTE: dst.sym (the callee name, set above) is only read
           by codegen's OP_LIBC_CALL case before it emits the
           call/bl instruction; overwriting dst.kind here to
           OPND_REG is safe because codegen never re-reads dst.sym
           after that point for this instruction. */
    }
}

int parse_instr_line(char *tokens[], int ntok, const char *raw_trimmed) {
    if (ntok == 0) return 0;

    /* Any line other than 'else {' or '} else {' means a pending
       if-close (see below) is definitely not followed by an else --
       finalize it now, before this line's own tokens are interpreted. */
    int line_is_else = (strcmp(tokens[0], "else") == 0) ||
                        (strcmp(tokens[0], "}") == 0 && ntok >= 2 && strcmp(tokens[1], "else") == 0);
    if (!line_is_else) finalize_pending_if_scope();

    /* closing brace of a block: for a plain @label: { ... } function
       block this remains a no-op (the flat v1 model). For a while-scope
       it closes immediately (while has no trailing 'else' to wait for).
       For an if-scope it's marked pending_close instead of finalized
       right away, since 'else {' may still follow -- either later as
       its own line, or right here as '} else {' on this same line (in
       which case tokens[0] is still '}', so this branch handles both:
       set pending_close, then let control fall through by re-dispatching
       the remaining 'else {' tokens on this same line below). */
    if (strcmp(tokens[0], "}") == 0) {
        if (scope_depth == 0) {
            /* closes the innermost open @label: { } block, unless
               'else' trails it, which would mean an else with no
               matching pending if. @label blocks may now nest (see
               open_local_frame's comment), so this closes exactly one
               level -- an outer block, if any, stays open and its own
               locals remain in scope for whatever code follows. */
            if (ntok >= 2 && strcmp(tokens[1], "else") == 0) fail("'else' with no matching 'if'");
            if (!in_local_frame()) fail("'}' with no matching '@label {' to close");
            close_local_frame();
            return 1;
        }
        if (scope_stack[scope_depth - 1].kind == SCOPE_WHILE) {
            scope_t s = scope_stack[--scope_depth];
            instr_t back; memset(&back, 0, sizeof(back));
            back.op = OP_JMP;
            back.dst.kind = OPND_LABEL;
            strncpy(back.dst.sym, s.top_label, MAX_SYMLEN - 1);
            push__instr(back);
            instr_t end; memset(&end, 0, sizeof(end));
            end.op = OP_LABEL;
            strncpy(end.dst.sym, s.end_label, MAX_SYMLEN - 1);
            push__instr(end);
            return 1;
        }
        if (scope_stack[scope_depth - 1].kind == SCOPE_FOR) {
            scope_t s = scope_stack[--scope_depth];
            /* continue_label: -- falls straight in here from the body's
               end (no jump needed since it's the very next instruction);
               'continue' inside the body jumps here explicitly instead
               (see the 'continue' handler above). Placed right before
               the increment so both paths -- falling off the body, or
               an explicit 'continue' -- run the increment exactly once
               per iteration before the condition is re-tested. */
            instr_t cont; memset(&cont, 0, sizeof(cont));
            cont.op = OP_LABEL;
            strncpy(cont.dst.sym, s.continue_label, MAX_SYMLEN - 1);
            push__instr(cont);

            /* VAR += STEP */
            instr_t inc; memset(&inc, 0, sizeof(inc));
            inc.op = OP_ADD;
            inc.dst = s.for_var;
            inc.src = s.for_step;
            push__instr(inc);

            instr_t back; memset(&back, 0, sizeof(back));
            back.op = OP_JMP;
            back.dst.kind = OPND_LABEL;
            strncpy(back.dst.sym, s.top_label, MAX_SYMLEN - 1);
            push__instr(back);

            instr_t end; memset(&end, 0, sizeof(end));
            end.op = OP_LABEL;
            strncpy(end.dst.sym, s.end_label, MAX_SYMLEN - 1);
            push__instr(end);
            return 1;
        }
        /* SCOPE_IF */
        scope_stack[scope_depth - 1].pending_close = 1;
        if (ntok >= 2 && strcmp(tokens[1], "else") == 0) {
            /* '} else {' on one line: re-dispatch as if 'else' were the
               whole line, using tokens[1..] and the raw text from the
               first non-'}' character onward */
            const char *else_part = raw_trimmed;
            while (*else_part == '}') else_part++;
            while (isspace((unsigned char)*else_part)) else_part++;
            char elsebuf[MAX_LINE];
            strncpy(elsebuf, else_part, sizeof(elsebuf) - 1);
            elsebuf[sizeof(elsebuf) - 1] = '\0';
            char *elsetoks[MAX_TOKENS];
            int elsen = tokenize(elsebuf, elsetoks);
            return parse_instr_line(elsetoks, elsen, else_part);
        }
        return 1;
    }

    /* 'else {' -- only valid immediately after an if-scope's closing
       '}', i.e. that scope is innermost and pending_close. Closes the
       if branch (jump to end, place the else label) so the else body's
       instructions land between else_label and end_label; the matching
       '}' for the else body finalizes end_label as usual. */
    if (strcmp(tokens[0], "else") == 0) {
        if (scope_depth == 0 || scope_stack[scope_depth - 1].kind != SCOPE_IF ||
            !scope_stack[scope_depth - 1].pending_close)
            fail("'else' with no matching 'if'");
        if (ntok < 2 || strcmp(tokens[ntok - 1], "{") != 0)
            fail("'else' must be followed by '{'");
        scope_t *s = &scope_stack[scope_depth - 1];
        s->has_else = 1;
        s->pending_close = 0; /* body reopened; next '}' closes for real */

        instr_t jmp_end; memset(&jmp_end, 0, sizeof(jmp_end));
        jmp_end.op = OP_JMP;
        jmp_end.dst.kind = OPND_LABEL;
        strncpy(jmp_end.dst.sym, s->end_label, MAX_SYMLEN - 1);
        push__instr(jmp_end);

        instr_t else_lbl; memset(&else_lbl, 0, sizeof(else_lbl));
        else_lbl.op = OP_LABEL;
        strncpy(else_lbl.dst.sym, s->else_label, MAX_SYMLEN - 1);
        push__instr(else_lbl);
        return 1;
    }

    /* assert LHS OP RHS; -- runtime postcondition. Same 'LHS OP RHS'
       grammar as if/while (see parse_cond_and_emit_cmp_ex), terminated
       by ';' instead of opening a '{' block: there's no body, just an
       inline checkpoint. If the condition is false at runtime, the
       program traps immediately; if true, execution falls straight
       through to the next statement. */
    if (strcmp(tokens[0], "assert") == 0) {
        const char *after = raw_trimmed + 6; /* skip "assert" */
        opcode_t jmp_op = parse_cond_and_emit_cmp_ex(after, 0);

        instr_t a; memset(&a, 0, sizeof(a));
        a.op = OP_ASSERT;
        a.assert_jmp_op = jmp_op;
        push__instr(a);
        return 1;
    }

    /* break; -- jumps straight to the innermost enclosing while/for's
       end label, exiting the loop immediately. Skips over any
       intervening if-scopes (see find_enclosing_loop_scope's comment)
       -- an if nested inside a loop is not itself a loop boundary. */
    if (strcmp(tokens[0], "break") == 0 || strcmp(tokens[0], "break;") == 0) {
        if (ntok != 1) fail("malformed 'break': expected 'break;'");
        scope_t *loop = find_enclosing_loop_scope();
        if (!loop) fail("'break' outside a loop");

        instr_t j; memset(&j, 0, sizeof(j));
        j.op = OP_JMP;
        j.dst.kind = OPND_LABEL;
        strncpy(j.dst.sym, loop->end_label, MAX_SYMLEN - 1);
        push__instr(j);
        return 1;
    }

    /* continue; -- jumps to the innermost enclosing loop's re-test
       point. For 'while' that's simply top_label (the condition is
       re-checked immediately). For 'for' it's continue_label, which
       runs the step/increment first and only then falls into the
       condition re-test -- jumping straight to top_label from a 'for'
       body would skip the increment and loop forever (or diverge from
       what the loop variable should be), so 'for' keeps a separate
       label for this. */
    if (strcmp(tokens[0], "continue") == 0 || strcmp(tokens[0], "continue;") == 0) {
        if (ntok != 1) fail("malformed 'continue': expected 'continue;'");
        scope_t *loop = find_enclosing_loop_scope();
        if (!loop) fail("'continue' outside a loop");

        instr_t j; memset(&j, 0, sizeof(j));
        j.op = OP_JMP;
        j.dst.kind = OPND_LABEL;
        const char *target = (loop->kind == SCOPE_FOR) ? loop->continue_label : loop->top_label;
        strncpy(j.dst.sym, target, MAX_SYMLEN - 1);
        push__instr(j);
        return 1;
    }

    /* if LHS OP RHS { ... } [else { ... }] -- see the block comment
       above parse_cond_and_emit_cmp for the full desugaring. Without an
       else, a false condition jumps straight to the end label; with an
       else, it jumps to the else label instead (set up here, wired to
       an actual body only if 'else {' is seen before the matching '}'). */
    if (strcmp(tokens[0], "if") == 0) {
        int id = g_label_counter++;
        scope_t s; memset(&s, 0, sizeof(s));
        s.kind = SCOPE_IF;
        snprintf(s.end_label, MAX_SYMLEN, ".Lif%d_end", id);
        snprintf(s.else_label, MAX_SYMLEN, ".Lif%d_else", id);

        const char *after = raw_trimmed + 2; /* skip "if" */
        opcode_t skip_op = parse_cond_and_emit_cmp(after);

        instr_t skip; memset(&skip, 0, sizeof(skip));
        skip.op = skip_op;
        skip.dst.kind = OPND_LABEL;
        /* false condition always jumps to else_label; if the matching
           '}' never sees an 'else {', that label is emitted as an empty
           block right before end_label (see the '}' handler above) */
        strncpy(skip.dst.sym, s.else_label, MAX_SYMLEN - 1);
        push__instr(skip);

        push__scope(s);
        return 1;
    }

    /* while LHS OP RHS { ... } -- re-checks the condition each
       iteration (not a do-while); a false condition on entry skips the
       body entirely, same as a C while loop. */
    if (strcmp(tokens[0], "while") == 0) {
        int id = g_label_counter++;
        scope_t s; memset(&s, 0, sizeof(s));
        s.kind = SCOPE_WHILE;
        snprintf(s.top_label, MAX_SYMLEN, ".Lwhile%d_top", id);
        snprintf(s.end_label, MAX_SYMLEN, ".Lwhile%d_end", id);

        instr_t top; memset(&top, 0, sizeof(top));
        top.op = OP_LABEL;
        strncpy(top.dst.sym, s.top_label, MAX_SYMLEN - 1);
        push__instr(top);

        const char *after = raw_trimmed + 5; /* skip "while" */
        opcode_t skip_op = parse_cond_and_emit_cmp(after);

        instr_t skip; memset(&skip, 0, sizeof(skip));
        skip.op = skip_op;
        skip.dst.kind = OPND_LABEL;
        strncpy(skip.dst.sym, s.end_label, MAX_SYMLEN - 1);
        push__instr(skip);

        push__scope(s);
        return 1;
    }

    /* for VAR = START, END[, STEP] { ... } -- counted loop, ascending
     * by default. Desugars into the equivalent 'mv VAR, START; while
     * VAR < END { ...; VAR += STEP }' (or 'VAR > END' for a negative
     * STEP -- see below) -- i.e. condition re-tested each iteration
     * exactly like 'while' (not a do-while), STEP defaults to 1 if
     * omitted, and the increment always runs through continue_label so
     * that 'continue' inside the body still advances VAR instead of
     * looping forever on the same value:
     *
     *   for r1 = 0, 10 {        mov r1, 0
     *       ...              => .Lfor0_top:
     *   }                        cmp r1, 10 ; jge .Lfor0_end
     *                            ...
     *                           .Lfor0_cont:
     *                            add r1, 1
     *                            jmp .Lfor0_top
     *                           .Lfor0_end:
     *
     * VAR must be a plain register (the same restriction 'if'/'while'
     * already place on their LHS -- see parse_cond_and_emit_cmp_ex);
     * END may be a register or an immediate, same as any other operand.
     * STEP, however, must be a compile-time immediate (nonzero): its
     * sign is what picks ascending ('<') vs. descending ('>') for the
     * bound check, and that choice has to be made once at compile time,
     * not per-iteration -- a register STEP whose sign isn't known until
     * runtime is rejected rather than silently assumed ascending. Write
     * a negative immediate STEP for a descending loop, e.g.
     * 'for r1 = 10, 0, -1 { ... }'. */
    if (strcmp(tokens[0], "for") == 0) {
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *term = strrchr(buf, '{');
        if (!term) fail("expected '{' to open 'for' body");
        *term = '\0';

        char *toks[MAX_TOKENS];
        int n = tokenize(buf, toks);
        /* 'for' VAR '=' START ',' END [',' STEP] -- tokenize() splits
           on whitespace only, so the commas arrive glued onto whichever
           operand token precedes them (same as the trailing ';' on any
           other statement -- see strip__semicolon's comment); each
           operand token below has its own trailing ',' (if any)
           stripped before being parsed. */
        if (n != 5 && n != 6) fail("malformed 'for': expected 'for VAR = START, END [, STEP] {'");
        if (strcmp(toks[0], "for") != 0) fail("malformed 'for'"); /* unreachable: dispatch already matched */
        if (strcmp(toks[2], "=") != 0) fail("malformed 'for': expected 'for VAR = START, END [, STEP] {'");

        int id = g_label_counter++;
        scope_t s; memset(&s, 0, sizeof(s));
        s.kind = SCOPE_FOR;
        snprintf(s.top_label, MAX_SYMLEN, ".Lfor%d_top", id);
        snprintf(s.continue_label, MAX_SYMLEN, ".Lfor%d_cont", id);
        snprintf(s.end_label, MAX_SYMLEN, ".Lfor%d_end", id);

        parse__operand(toks[1], &s.for_var);
        if (s.for_var.kind != OPND_REG)
            fail("'for' loop variable must be a register (for rN = START, END [, STEP])");

        char start_tok[MAX_SYMLEN]; strncpy(start_tok, toks[3], MAX_SYMLEN - 1); start_tok[MAX_SYMLEN - 1] = '\0';
        strip__trailing_comma(start_tok);
        operand_t start_opnd; parse__operand(start_tok, &start_opnd);

        /* END has a trailing ',' to strip only when STEP follows (n==6);
           with no STEP (n==5), END is directly followed by the '{' that
           term already cut off above, so there's nothing to strip. */
        char end_tok[MAX_SYMLEN];
        strncpy(end_tok, toks[4], MAX_SYMLEN - 1); end_tok[MAX_SYMLEN - 1] = '\0';
        if (n == 6) strip__trailing_comma(end_tok);
        operand_t end_opnd; parse__operand(end_tok, &end_opnd);

        if (n == 6) {
            char step_tok[MAX_SYMLEN];
            strncpy(step_tok, toks[5], MAX_SYMLEN - 1); step_tok[MAX_SYMLEN - 1] = '\0';
            parse__operand(step_tok, &s.for_step);
        } else {
            s.for_step.kind = OPND_IMM;
            s.for_step.imm = 1; /* default step */
        }

        /* Loop direction (ascending vs. descending) has to be known at
           compile time, since it picks which comparison ('<' vs '>')
           the bound check below emits -- there is no single comparison
           that is correct for both a positive and a negative step. This
           is only decidable when STEP is a compile-time immediate: a
           register STEP's sign isn't known until runtime, and silently
           assuming ascending in that case would make 'for r1 = 10, 0, r3 {'
           silently never execute if r3 turns out negative -- exactly
           the kind of silent-wrong-answer failure this codebase avoids
           elsewhere (see parse_mem_order_suffix's comment on the same
           tradeoff). So a register STEP is rejected outright, and a
           zero immediate STEP (which would never reach END at all) is
           rejected too, rather than compiling into an infinite loop. */
        if (s.for_step.kind != OPND_IMM)
            fail("'for' loop STEP must be a compile-time immediate (its sign selects ascending vs. descending) -- a register STEP's direction isn't known until runtime");
        if (s.for_step.imm == 0)
            fail("'for' loop STEP must not be 0 -- that would never reach END");
        int ascending = s.for_step.imm > 0;

        /* mov VAR, START */
        instr_t init; memset(&init, 0, sizeof(init));
        init.op = OP_MOV;
        init.dst = s.for_var;
        init.src = start_opnd;
        push__instr(init);

        /* top: */
        instr_t top; memset(&top, 0, sizeof(top));
        top.op = OP_LABEL;
        strncpy(top.dst.sym, s.top_label, MAX_SYMLEN - 1);
        push__instr(top);

        /* cmp VAR, END ; jump-to-end on the inverted condition -- '<'
           (exit once VAR >= END) for an ascending/positive step, '>'
           (exit once VAR <= END) for a descending/negative step. */
        instr_t cmp; memset(&cmp, 0, sizeof(cmp));
        cmp.op = OP_CMP;
        cmp.dst = s.for_var;
        cmp.src = end_opnd;
        push__instr(cmp);

        instr_t skip; memset(&skip, 0, sizeof(skip));
        skip.op = invert_cond_op(ascending ? "<" : ">", 0);
        skip.dst.kind = OPND_LABEL;
        strncpy(skip.dst.sym, s.end_label, MAX_SYMLEN - 1);
        push__instr(skip);

        push__scope(s);
        return 1;
    }

    /* @name {, @name: {, @name, or @name: -- a label. The label
       literally named 'start' is the program's entry point; any other
       @label is an ordinary function/jump target. The trailing ':' is
       now optional (new-style '@name {'); when present it's still
       accepted and simply ignored, so existing '@name: {' source keeps
       working unchanged. */
    if (tokens[0][0] == '@') {
        /* Reconstruct the full label/signature text from raw_trimmed
           rather than tokens[]: tokenize() only splits on whitespace,
           so '@add(a, b) -> r1 {' (spaces around the comma and '->')
           arrives as several separate tokens, not one glued-together
           tokens[0] the way a plain '@name' does. */
        const char *at = strchr(raw_trimmed, '@');
        if (!at) fail("malformed label"); /* unreachable: tokens[0][0]=='@' guarantees this */
        const char *paren = strchr(at, '(');

        /* 'end_scan_start' is where the name (no-params case) or the
           '-> rN' return register (params case) is known to terminate:
           the first of '{', ';', ':', or end-of-string from that
           point on. Using this instead of requiring a literal ':'
           (the old behavior) is what makes the trailing colon
           optional: '@name {' terminates the name at '{' exactly the
           way '@name: {' terminates it at ':' -- same scan, just a
           different stop character reached first. split__statements'
           own '@'-region skip (see its comment) has already consumed
           any old-style ':' out of this chunk by the time this code
           runs in the normal pipeline, but scanning for it here too
           keeps this function correct if ever called with an
           un-pre-split raw line (e.g. future direct callers, tests). */
        const char *colon_scan_start = paren ? paren : at;
        const char *term = colon_scan_start;
        while (*term && *term != '{' && *term != ';' && *term != ':') term++;
        const char *colon = (*term == ':') ? term : NULL;
        const char *name_end = colon ? colon : term; /* first of ':' / '{' / ';' / '\0' */

        char label[MAX_SYMLEN];
        int has_params = (paren && paren < name_end);

        if (!has_params) {
            /* Plain '@name', '@name:', '@name {', or '@name: {' --
               the name runs up to whichever terminator was found. */
            size_t namelen = (size_t)(name_end - (at + 1));
            /* Trim trailing whitespace between the name and its
               terminator (e.g. '@name {' with a space before '{'). */
            while (namelen > 0 && isspace((unsigned char)(at + 1)[namelen - 1])) namelen--;
            if (namelen == 0 || namelen >= MAX_SYMLEN) fail("malformed label: empty or too-long name after '@'");
            memcpy(label, at + 1, namelen);
            label[namelen] = '\0';
        } else {
            /* '@name(p1, p2, ...) -> rN' (':' optional) -- a function
               declaration. Parameters bind positionally to r1, r2,
               r3... (see the "Function parameters" section) and rN
               documents which register the caller should read the
               return value from; ret; itself does not populate it
               automatically -- the function body must move its result
               there before returning, the same explicitness Chard
               already expects everywhere else. */
            size_t namelen = (size_t)(paren - (at + 1));
            if (namelen == 0 || namelen >= MAX_SYMLEN) fail("malformed function: empty or too-long name after '@'");
            memcpy(label, at + 1, namelen);
            label[namelen] = '\0';
            if (strcmp(label, "start") == 0) fail("'@start' cannot take parameters (it is the program entry point, never called)");
            if (strcmp(label, "main") == 0) fail("'@main' cannot take parameters (it is the program entry point, never called)");

            if (in_function) fail("functions cannot be declared inside another function's body (nested @label blocks may still be used for local-variable scoping, just not another parameter list)");
            if (in_local_frame()) fail("functions may only be declared at file scope, not nested inside another @label block");

            const char *close_paren = strchr(paren, ')');
            if (!close_paren || close_paren > name_end) fail("malformed function: missing ')' before the return arrow / label end");

            param_scope_t ps; memset(&ps, 0, sizeof(ps));
            char paramtext[256];
            size_t plen = (size_t)(close_paren - (paren + 1));
            if (plen >= sizeof(paramtext)) fail("too many characters in parameter list");
            memcpy(paramtext, paren + 1, plen);
            paramtext[plen] = '\0';

            char *ptok = strtok(paramtext, ", ");
            while (ptok) {
                if (ps.nparams >= MAX_PARAMS) fail("too many parameters (max 12, matching r1-r12)");
                strncpy(ps.names[ps.nparams], ptok, MAX_SYMLEN - 1);
                ps.nparams++;
                ptok = strtok(NULL, ", ");
            }

            const char *arrow = strstr(close_paren, "->");
            if (!arrow || arrow > name_end) fail("malformed function: expected '-> rN' return register after ')'");
            char retbuf[16];
            const char *retstart = arrow + 2;
            while (*retstart == ' ') retstart++;
            size_t retlen = (size_t)(name_end - retstart);
            while (retlen > 0 && isspace((unsigned char)retstart[retlen - 1])) retlen--;
            if (retlen == 0 || retlen >= sizeof(retbuf)) fail("malformed function: expected a register (e.g. r1) after '->'");
            memcpy(retbuf, retstart, retlen);
            retbuf[retlen] = '\0';
            operand_t retop;
            parse__operand(retbuf, &retop); /* validated below: must resolve to a plain register */
            if (retop.kind != OPND_REG || retop.is_sp) fail("'-> RETVAL' must name a register (e.g. r1), not sp or anything else");

            current_params = ps;
            in_function = 1;

            if (find_func_sig(label)) failf("redeclaration of function '%s'", label);
            DA_ENSURE(func_sigs, func_sigs_cap, nfunc_sigs, func_sig_t);
            func_sig_t *sig = &func_sigs[nfunc_sigs++];
            strncpy(sig->name, label, MAX_SYMLEN - 1);
            sig->nparams = ps.nparams;
            sig->ret_reg = retop.reg_num;
        }

        /* '@main' is sugar for '@start' plus an implicit leading
           'libc-init;': it's still THE entry block (is_entry, the
           '_start'-by-default dst.sym, the 'global'/global-pin
           machinery that keys off is_entry -- see e.g.
           check_global_pin_violations) exactly like '@start', but it
           also does what a hand-written 'libc-init;' as the block's
           first statement would do: set g_libc_linked/g_libc_init_seen
           and emit OP_LIBC_INIT so codegen seeds the libc-linked
           'main' prologue before any user code runs (same "seed it
           once, right at the top" ordering OP_LIBC_INIT's own comment
           describes). This only saves writing 'libc-init;' by hand --
           apply_entry_symbol_override still does the actual _start->
           main rename off g_libc_linked the normal way, so '%entrysym'
           still overrides it exactly as it would for '@start' +
           'libc-init;'. */
        int is_main_shorthand = (strcmp(label, "main") == 0);
        int is_entry = (strcmp(label, "start") == 0) || is_main_shorthand;
        if (is_entry) strncpy(entry_label, "_start", MAX_SYMLEN - 1);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_LABEL;
        i.is_entry = is_entry;
        strncpy(i.dst.sym, is_entry ? "_start" : label, MAX_SYMLEN - 1);
        int label_instr_idx = nprog; /* see opens_block below: back-filled
                                         once we know this label opens a
                                         function-root body, for CFI */
        push__instr(i);

        if (is_main_shorthand) {
            if (g_mode == MODE_BARE) fail("'@main' requires '| mode elf;' -- a BARE/raw target has no libc to link against "
                                           "(use '@start' if you want a freestanding entry point)");
            g_libc_linked = 1;
            g_libc_init_seen = 1;
            g_entry_is_main_shorthand = 1;
            instr_t li; memset(&li, 0, sizeof(li));
            li.op = OP_LIBC_INIT;
            push__instr(li);
        }

        /* '@name: {' or '@name(...) -> rN: {' opens a block -- and with
           it, a fresh frame for any 'local' declarations inside. A bare
           '@name:' with no trailing '{' is just a jump target with no
           body of its own, so it never opens a frame (and,
           symmetrically, can have no matching '}' to close one). A
           function declaration always requires a body (there's nowhere
           else for its parameter aliases to make sense), so a
           parameter list with no trailing '{' is an error rather than
           silently accepted. */
        int opens_block = (ntok >= 1 && strcmp(tokens[ntok - 1], "{") == 0);
        if (has_params && !opens_block) fail("function declaration requires a body: '@name(...) -> rN: { ... }'");
        if (opens_block) {
            open_local_frame();
            {
                /* Any '@name: { ... }' block opens its own frame and is
                   independently callable via 'call @name;', with the same
                   "must end in ret;" stack-discipline requirement as a
                   parameterized function (see close_local_frame's
                   caller). It therefore needs the same callee-save
                   wrapping and CFI treatment too, whether or not it
                   declared a '(params) -> rN' signature -- func_sig stays
                   NULL for the plain case, and wrap_function_body/CFI
                   emission both already treat a NULL func_sig as "no
                   params, no return register" rather than requiring one. */
                local_frame_t *lf = current_local_frame();
                lf->is_function_root = 1;
                lf->func_sig = has_params ? find_func_sig(label) : NULL;
                lf->label_instr_idx = label_instr_idx;
                prog[label_instr_idx].is_func_start = 1;
            }
            /* extern's scope restriction (see g_in_main_block's comment)
               only makes sense once @main actually has an open body to
               restrict externs to -- a bodyless '@main:' with no '{' is
               a jump target, not a libc-linked entry point, and can't
               contain anything, extern included. */
            if (is_main_shorthand) {
                g_in_main_block = 1;
                g_main_block_frame_depth = local_frame_depth;
            }
        }
        return 1;
    }

    /* ret */
    if (strcmp(tokens[0], "ret") == 0 || strcmp(tokens[0], "ret;") == 0) {
        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_RET;
        push__instr(i);
        return 1;
    }

    /* extern name(nargs);  or  extern name(nargs) lib "libname";
       -- declares an external symbol and its argument count, so later
       'libcall name(...)' sites can be validated the same way call()
       is validated against func_sigs. Purely a declaration: emits no
       instruction of its own, just registers the signature and (at
       codegen time) a target-correct "this symbol is external" line.
       See the "libc interop" block comment above find_extern for the
       full picture.

       The optional trailing 'lib "libname";' is "extern library tagging":
       it doesn't change codegen for the symbol itself (an extern/.extern
       directive looks the same either way -- the assembler doesn't care
       *which* library eventually satisfies it, only that it's external),
       but it records that this symbol needs '-llibname' at link time, the
       same way libc symbols need nothing beyond the ordinary C runtime
       link. Every tagged lib is collected into g_extern_libs and surfaced
       as a suggested link line after codegen (see the end of main()),
       exactly how BARE mode's '| foot' already gets a "here's the build
       command you need" note instead of Chard silently trying to invoke
       ld/cc itself -- Chard stays text-out only (see the file banner
       comment), it just now tells you the *right* text-out build command
       when a real third-party C library is involved, e.g.:
           extern sqrt(1) lib "m";
           extern curl_easy_init(0) lib "curl";
       and not just bare libc:
           extern printf(2);   // no 'lib' tag -- ordinary libc, always linked
    */
    if (strncmp(tokens[0], "extern", 6) == 0 && (tokens[0][6] == '\0' || tokens[0][6] == ';')) {
        if (!g_in_main_block) fail("'extern' may only appear inside '@main { ... }' -- it declares an ABI "
                                    "contract for a libc-linked program, and '@main' is the only place a "
                                    "program becomes libc-linked, so an extern anywhere else has no libc-linked "
                                    "entry point to belong to");
        if (ntok < 2) fail("malformed extern: expected 'extern name(nargs);'");
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char *open = strchr(buf, '(');
        char *close = open ? strrchr(buf, ')') : NULL;
        if (!open || !close || close < open) fail("malformed extern: expected 'extern name(nargs);'");
        char *name_start = buf + 6;
        while (*name_start == ' ') name_start++;
        size_t namelen = (size_t)(open - name_start);
        while (namelen > 0 && name_start[namelen - 1] == ' ') namelen--;
        if (namelen == 0 || namelen >= MAX_SYMLEN) fail("malformed extern: missing function name");
        char name[MAX_SYMLEN];
        memcpy(name, name_start, namelen);
        name[namelen] = '\0';
        if (find_extern(name)) failf("redeclaration of extern '%s'", name);
        DA_ENSURE(externs, externs_cap, nexterns, extern_sig_t);

        char argbuf[64];
        strncpy(argbuf, open + 1, (size_t)(close - open - 1) < sizeof(argbuf) - 1 ? (size_t)(close - open - 1) : sizeof(argbuf) - 1);
        argbuf[(size_t)(close - open - 1) < sizeof(argbuf) - 1 ? (size_t)(close - open - 1) : sizeof(argbuf) - 1] = '\0';
        char *argtrim = trim(argbuf);
        if (*argtrim != '\0' && !is__number(argtrim)) {
            char msg[192];
            snprintf(msg, sizeof(msg), "extern '%s': argument count must be a plain integer literal (got '%s')", name, argtrim);
            fail(msg);
        }
        int na = (*argtrim == '\0') ? 0 : (int)parse__number(argtrim);
        if (na < 0 || na > MAX_LIBC_ARGS) {
            char msg[128];
            snprintf(msg, sizeof(msg), "extern '%s': nargs must be between 0 and %d", name, MAX_LIBC_ARGS);
            fail(msg);
        }

        /* Optional 'lib "name";' tag after the '(nargs)' part. Scan for it
           in whatever's left of the line past the closing paren, rather
           than relying on the tokens[] split (a quoted string containing
           spaces would otherwise land across multiple tokens). */
        char libname[MAX_SYMLEN]; libname[0] = '\0';
        char *after = close + 1;
        while (*after == ' ') after++;
        if (strncmp(after, "lib", 3) == 0 && (after[3] == ' ' || after[3] == '"')) {
            char *p = after + 3;
            while (*p == ' ') p++;
            if (*p != '"') failf("extern '%s': expected a quoted library name after 'lib', e.g. lib \"m\";", name);
            p++;
            char *qend = strchr(p, '"');
            if (!qend) failf("extern '%s': unterminated string in 'lib \"...\"'", name);
            size_t liblen = (size_t)(qend - p);
            if (liblen == 0) failf("extern '%s': 'lib \"\"' -- library name can't be empty", name);
            if (liblen >= MAX_SYMLEN) failf("extern '%s': library name too long", name);
            memcpy(libname, p, liblen);
            libname[liblen] = '\0';
        } else if (*after != '\0' && *after != ';') {
            char msg[192];
            snprintf(msg, sizeof(msg), "extern '%s': unexpected trailing text '%s' -- expected ';' or 'lib \"name\";'", name, after);
            fail(msg);
        }

        extern_sig_t *sig = &externs[nexterns++];
        strncpy(sig->name, name, MAX_SYMLEN - 1);
        sig->nargs = na;
        strncpy(sig->lib, libname, MAX_SYMLEN - 1);
        sig->lib[MAX_SYMLEN - 1] = '\0';
        if (libname[0]) note_extern_lib(libname);
        return 1;
    }

    /* libc-init;  -- one-time marker required before any 'libcall',
       conventionally the first statement in @entry. Switches the whole
       program from a freestanding _start/raw-syscall entry point to a
       libc-linked 'main' entry point -- see the "libc interop" block
       comment and each backend's entry-point emission for what
       actually changes. Emits its own pseudo-instruction (rather than
       just setting g_libc_linked at parse time with no IR trace) so
       codegen can place it precisely: it must be the very first thing
       in the emitted 'main', before any user code runs, exactly the
       same "seed it once, right at the top" requirement g_uses_heap's
       __heap_ptr seeding already has for @entry. */
    if (strcmp(tokens[0], "libc-init") == 0 || strcmp(tokens[0], "libc-init;") == 0) {
        /* '@main { ... }' (see the '@'-label parse site) already does
           everything 'libc-init;' does -- sets g_libc_linked/
           g_libc_init_seen and pushes OP_LIBC_INIT -- the instant the
           block opens, so that libc-call/libc-heap-* are usable from
           the very first statement without also requiring a redundant
           hand-written 'libc-init;' first. A programmer who writes
           'libc-init;' anyway out of habit (e.g. copying an @start
           block over to @main) shouldn't hit the ordinary
           may-only-appear-once error for it, since from their
           perspective they only wrote it once -- so this is a no-op
           warning specifically when @main already covered it, and
           still the normal hard error for an actual double
           'libc-init;'/'libc-init;' inside @start. */
        if (g_libc_init_seen) {
            if (g_entry_is_main_shorthand)
                fprintf(stderr, "warning: 'libc-init;' is redundant inside '@main { ... }' "
                                 "(the block header already does this) -- ignoring\n");
            else
                fail("'libc-init' may only appear once");
            return 1;
        }
        if (g_mode == MODE_BARE) fail("'libc-init' requires '| mode elf;' -- a BARE/raw target has no libc to link against");
        g_libc_linked = 1;
        g_libc_init_seen = 1;
        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_LIBC_INIT;
        push__instr(i);
        return 1;
    }

    /* libcall name(arg1, ..., arg6) [> rX];  -- calls an externally
       declared (via 'extern') libc function using the target's real C
       ABI, not Chard's r1-r12 convention. Requires 'libc-init;' to have
       already run (a libc function called before libc's own startup
       has initialized things like malloc's arena or stdio's buffers is
       liable to crash or behave unpredictably -- Chard enforces the
       ordering at compile time rather than letting that surprise
       happen at runtime). Argument marshalling mirrors call()'s shape
       (left to right, one at a time, no hidden temporaries to break
       register cycles -- same explicitness contract) but lands
       arguments in each target's actual C ABI registers instead of
       r1-r12; see each backend's OP_LIBC_CALL case for the exact
       registers. */
    /* Shared body for both the explicit 'libc-call NAME(args) [> rX];'
       keyword form (below) and the implicit bare 'NAME(args) [> rX];'
       fallback (see the end of this function, right before its final
       'return 0'): once libc-init has run, requiring the 'libc-call'
       keyword in front of every single call to a declared extern is
       needless ceremony -- the extern declaration already told Chard
       everything it needs (the name and arity), so a bare call can be
       recognized as "this must be a libc-call" the same way call()
       already recognizes a bare '@name(args)' site. parse_libc_call_body
       (defined above parse_instr_line) takes 'name_start' pointing at
       where the callee name begins in raw_trimmed -- right after the
       'libc-call ' keyword for the explicit form, or the very start of
       the line for the implicit form -- so both forms parse through
       exactly one path: same extern lookup, same arity check, same
       optional '> rX' capture. There is exactly one source of truth for
       what a libc-call instruction looks like, just two ways to spell
       its entry point.

       'libc-call' the keyword is now INVALID everywhere except the one
       case it exists to solve: a name that is simultaneously a declared
       'extern' and a Chard '@name(...) -> rN' function, where bare
       'NAME(args);' is genuinely ambiguous between the two and the
       implicit fallback below refuses to guess. Everywhere else, the
       bare form is the only spelling -- writing the keyword on a
       non-colliding extern is now a hard error pointing the programmer
       at the bare form instead, rather than silently accepting a
       needless keyword. See the collision check inside
       parse_libc_call_body's caller here, and the matching comment on
       the implicit fallback below, for the other half of this rule. */

    if (strncmp(tokens[0], "libc-call", 9) == 0 && (tokens[0][9] == '\0') && ntok >= 2 && strchr(tokens[1], '(')) {
        if (!g_libc_init_seen) fail("'libc-call' used before 'libc-init;' -- libc-init must run first (conventionally as the first statement in @entry)");
        const char *name_start = raw_trimmed + 9;
        while (*name_start == ' ') name_start++;
        char cbuf[MAX_SYMLEN]; cbuf[0] = '\0';
        {
            const char *p = name_start;
            size_t k = 0;
            while (*p && *p != '(' && k < MAX_SYMLEN - 1) { cbuf[k++] = *p++; }
            cbuf[k] = '\0';
            char *ctrim = trim(cbuf);
            if (ctrim != cbuf) memmove(cbuf, ctrim, strlen(ctrim) + 1);
        }
        if (!(find_extern(cbuf) && find_func_sig(cbuf))) {
            char msg[256];
            snprintf(msg, sizeof(msg), "'libc-call' is no longer valid here -- '%s' isn't both an extern and a Chard function, so just write '%s(...);' with no prefix", cbuf, cbuf);
            fail(msg);
        }
        parse_libc_call_body(name_start, "libc-call");
        return 1;
    }

    /* exit(N) / exit(rN) -- accepts either an integer literal or an
       integer register holding the exit code at runtime. Must be one
       or the other: unlike the old parse__number(buf) call (which
       silently returned 0 for anything it couldn't parse, so
       'exit(r7)' compiled clean into 'exit(0)' with no warning), a
       token that's neither a recognized number nor a valid register
       is a hard parse error -- matching every other "no silent
       fallback" check elsewhere in this file (width mismatches,
       out-of-range registers, %section on a data-array decl, etc). */
    if (strncmp(tokens[0], "exit(", 5) == 0) {
        if (g_mode == MODE_BARE) fail("'exit()' is a kernel syscall wrapper and is not available in '| mode bare;' (the default) -- add '| mode elf;' at the top of the file to use it");
        char buf[64];
        strncpy(buf, tokens[0] + 5, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char *paren = strchr(buf, ')');
        if (!paren) fail("malformed exit(): missing ')'");
        *paren = '\0';
        char *argtok = trim(buf);
        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_EXIT;
        if (is__number(argtok)) {
            i.src.kind = OPND_IMM;
            i.src.imm = parse__number(argtok);
        } else {
            operand_t regop; memset(&regop, 0, sizeof(regop));
            if (!parse__register(argtok, &regop) || regop.is_float || regop.is_sp) {
                char msg[128];
                snprintf(msg, sizeof(msg), "malformed exit(): '%s' is neither an integer literal nor an integer register (r1-r12)", argtok);
                fail(msg);
            }
            i.src = regop;
        }
        push__instr(i);
        return 1;
    }

    /* halt; -- BARE mode's own process-terminating primitive; see
       OP_HALT's declaration for the full rationale. The mirror image
       of exit()'s mode gate above: exit() requires ELF (it's a
       syscall wrapper with no kernel to call into under BARE), halt
       requires BARE (ELF mode already has exit() for "stop the
       process," and a raw 'hlt'/'wfi' loop dropped into a
       libc/kernel-linked ELF binary would just hang the process
       instead of terminating it, which is not what a programmer
       reaching for a "halt" keyword there would want). */
    if (strcmp(tokens[0], "halt") == 0 || strcmp(tokens[0], "halt;") == 0) {
        if (g_mode != MODE_BARE) fail("'halt;' is a raw CPU halt and is only available in '| mode bare;' -- use 'exit(N);' to terminate a '| mode elf;' program");
        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_HALT;
        push__instr(i);
        return 1;
    }

    /* out(msg) -- named builtin: writes an ascii-declared string to
       file descriptor 1, using the string's compiler-tracked length.
       'msg' may be a 'volatile'/'bss' ascii symbol (src stays OPND_SYM,
       dst left unused -- codegen derives the length from the linker
       symbol 'msg_len', see each backend's OP_STDOUT case) or a 'local
       ascii' buffer (src becomes an OPND_LOCAL for the buffer itself,
       dst an OPND_LOCAL for its 'msg_len' companion local -- see the
       'local ascii' parsing above for why that companion is a second
       real local rather than a naming convention here, unlike the
       global case). Both share one instr_t/opcode; only the operand
       kinds differ, so every backend's OP_STDOUT case switches on
       ins->src.kind rather than needing a second opcode. */
    if (strncmp(tokens[0], "out(", 4) == 0) {
        if (g_mode == MODE_BARE) fail("'out()' is a kernel syscall wrapper (write) and is not available in '| mode bare;' (the default) -- add '| mode elf;' at the top of the file to use it");
        char buf[MAX_SYMLEN];
        strncpy(buf, tokens[0] + 4, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char *paren = strchr(buf, ')');
        if (!paren) fail("malformed out(): missing ')'");
        *paren = '\0';
        decl_t *outd = find__decl(buf);
        if (!outd) failf("out(): unknown symbol '%s'", buf);
        if (outd->section == SEC_LOCAL && !outd->is_ascii) failf("out(): '%s' is a local, not an ascii string", buf);
        if (outd->section != SEC_LOCAL && !outd->is_ascii) failf("out(): '%s' is not an ascii string", buf);
        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_STDOUT;
        if (outd->section == SEC_LOCAL) {
            parse__operand(buf, &i.src);
            /* buf's own successful declaration as 'local ascii' already
               enforced strlen(buf)+4 < MAX_SYMLEN (see that parsing
               above), so this can't actually truncate -- the check is
               repeated here only to keep this snprintf provably safe
               on its own, without relying on a fact proven at a
               distant call site. */
            if (strlen(buf) + 4 >= MAX_SYMLEN) fail("internal error: 'local ascii' name too long once '_len' is appended");
            char lenname[MAX_SYMLEN];
            strncpy(lenname, buf, MAX_SYMLEN - 1);
            lenname[MAX_SYMLEN - 1] = '\0';
            strncat(lenname, "_len", MAX_SYMLEN - 1 - strlen(lenname));
            decl_t *lend = find__decl(lenname);
            if (!lend) fail("internal error: 'local ascii' missing its length companion");
            parse__operand(lenname, &i.dst);
        } else {
            i.src.kind = OPND_SYM;
            strncpy(i.src.sym, buf, MAX_SYMLEN - 1);
        }
        push__instr(i);
        return 1;
    }

    /* alloc(N) > rX -- bump-allocates N bytes from the static heap arena
       and puts the base address of the new block in rX. N may be an
       immediate, a register, or a symbol (whose stored value is read at
       runtime). There is no per-block free(): v1's heap is arena-only,
       matching the rest of Chard's philosophy of exposing simple,
       predictable primitives rather than a full allocator. Memory *is*
       reclaimable, just all at once -- see 'hp-reset;' immediately
       below, which is the only form of reclamation an arena naturally
       supports without a free-list. */
    if (strncmp(tokens[0], "alloc(", 6) == 0) {
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char *open = strchr(buf, '(');
        char *close = strchr(buf, ')');
        if (!open || !close || close < open) fail("malformed alloc(): missing '(' or ')'");
        *close = '\0';
        char *sizetok = trim(open + 1);
        if (*sizetok == '\0') fail("alloc() requires a size argument");

        char *gt = strchr(close + 1, '>');
        if (!gt) fail("'alloc' expects 'alloc(N) > rX' syntax");
        char *dsttok = trim(gt + 1);
        strip__semicolon(dsttok);
        if (*dsttok == '\0') fail("'alloc' expects a destination register");

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_ALLOC;
        parse__operand(sizetok, &i.src);
        parse__operand(dsttok, &i.dst);
        if (i.dst.kind != OPND_REG) fail("'alloc' requires a register as its destination (alloc(N) > rX)");
        push__instr(i);
        g_uses_heap = 1;
        return 1;
    }

    /* hp-reset; -- reclaims every block ever handed out by alloc() in
       one step, by rewinding __heap_ptr back to &__heap. This is the
       arena's whole reclamation story: since alloc() never tracks
       individual block sizes or a free-list, there's no way to release
       just one block -- only "release everything allocated since the
       program (or since the last hp-reset;) started". Any pointer
       obtained from alloc() before a hp-reset; is invalid to use
       afterward (the bytes it pointed to will be handed out again by
       future alloc() calls); Chard does not track this for you, same as
       alloc() itself doing no bounds checking. Takes no operands and
       fails to parse ('hp-reset;' has never been used, so nothing to
       reclaim) unless the program has already used alloc() somewhere. */
    if (strcmp(tokens[0], "hp-reset") == 0 || strcmp(tokens[0], "hp-reset;") == 0) {
        if (!g_uses_heap) fail("'hp-reset' used before any 'alloc()' -- nothing to reclaim");
        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_HEAP_RESET;
        push__instr(i);
        return 1;
    }

    /* libc-heap-alloc(N) > rX;  /  libc-heap-free(rX);  /
       libc-heap-realloc(rPTR, N) > rX;

       A second, genuinely dynamic heap, distinct from alloc()'s fixed-
       size static arena (see DEFAULT_HEAP_SIZE_BYTES / '%sheap'
       above): alloc() can never grow past whatever size the arena was
       given at compile time, by design (a static arena is the only
       way to keep allocation semantics identical across x86-64/
       AArch64/RISC-V without diverging per-target brk/mmap syscall
       plumbing -- see the comment there). libc-heap-* sidesteps that
       limitation entirely by handing the actual allocation off to the
       host libc's own malloc/free/realloc, which already knows how to
       grow -- but that means it inherits libc's requirement of being
       linked, hence only available after 'libc-init;' (checked below,
       same requirement 'libc-call' itself already enforces).

       This is pure sugar over the existing extern/libcall machinery
       (see ensure_libc_heap_extern and OP_LIBC_CALL): each form below
       auto-registers the libc symbol it needs (so the programmer
       doesn't have to also hand-write 'extern malloc(1);' etc.) and
       then builds exactly the instr_t a hand-written
       'libcall malloc(N) > rX;' would, reusing OP_LIBC_CALL's codegen
       unchanged on every backend. A pointer returned by
       libc-heap-alloc/realloc must be released with libc-heap-free,
       not hp-reset (which only knows how to rewind alloc()'s arena) --
       the two heaps are entirely separate allocators and mixing a
       pointer from one into the other's reclamation is a use-after-
       free/double-free waiting to happen, the same way mixing malloc'd
       and stack-allocated pointers would be in C. */
    if (strncmp(tokens[0], "libc-heap-alloc(", 16) == 0) {
        if (!g_libc_init_seen) fail("'libc-heap-alloc' used before 'libc-init;' -- libc-init must run first (conventionally as the first statement in @entry)");
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char *open = strchr(buf, '(');
        char *close = strchr(buf, ')');
        if (!open || !close || close < open) fail("malformed libc-heap-alloc(): missing '(' or ')'");
        *close = '\0';
        char *sizetok = trim(open + 1);
        if (*sizetok == '\0') fail("libc-heap-alloc() requires a size argument");

        char *gt = strchr(close + 1, '>');
        if (!gt) fail("'libc-heap-alloc' expects 'libc-heap-alloc(N) > rX' syntax");
        char *dsttok = trim(gt + 1);
        strip__semicolon(dsttok);
        if (*dsttok == '\0') fail("'libc-heap-alloc' expects a destination register");

        operand_t dstop;
        parse__operand(dsttok, &dstop);
        if (dstop.kind != OPND_REG || dstop.is_sp) fail("'libc-heap-alloc' requires a register as its destination (libc-heap-alloc(N) > rX)");

        ensure_libc_heap_extern("malloc", 1);

        instr_t c; memset(&c, 0, sizeof(c));
        c.op = OP_LIBC_CALL;
        strncpy(c.dst.sym, "malloc", MAX_SYMLEN - 1); /* see OP_LIBC_CALL's
            own comment on dst.sym doubling as the callee name */
        parse__operand(sizetok, &c.args[0]);
        c.nargs = 1;
        push__instr(c);

        instr_t *libc_ins = &prog[nprog - 1];
        libc_ins->dst.kind = OPND_REG;
        libc_ins->dst.reg_num = dstop.reg_num;
        return 1;
    }

    if (strncmp(tokens[0], "libc-heap-free(", 15) == 0) {
        if (!g_libc_init_seen) fail("'libc-heap-free' used before 'libc-init;' -- libc-init must run first (conventionally as the first statement in @entry)");
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char *open = strchr(buf, '(');
        char *close = strchr(buf, ')');
        if (!open || !close || close < open) fail("malformed libc-heap-free(): missing '(' or ')'");
        *close = '\0';
        char *ptrtok = trim(open + 1);
        if (*ptrtok == '\0') fail("libc-heap-free() requires a pointer argument");

        ensure_libc_heap_extern("free", 1);

        instr_t c; memset(&c, 0, sizeof(c));
        c.op = OP_LIBC_CALL;
        strncpy(c.dst.sym, "free", MAX_SYMLEN - 1);
        parse__operand(ptrtok, &c.args[0]);
        c.nargs = 1;
        push__instr(c);
        return 1;
    }

    if (strncmp(tokens[0], "libc-heap-realloc(", 18) == 0) {
        if (!g_libc_init_seen) fail("'libc-heap-realloc' used before 'libc-init;' -- libc-init must run first (conventionally as the first statement in @entry)");
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char *open = strchr(buf, '(');
        char *close = strchr(buf, ')');
        if (!open || !close || close < open) fail("malformed libc-heap-realloc(): missing '(' or ')'");
        *close = '\0';
        char *argstok = trim(open + 1);
        if (*argstok == '\0') fail("libc-heap-realloc() requires 'rPTR, N' arguments");

        char *comma = strchr(argstok, ',');
        if (!comma) fail("'libc-heap-realloc' expects 'libc-heap-realloc(rPTR, N) > rX' syntax");
        *comma = '\0';
        char *ptrtok = trim(argstok);
        char *sizetok = trim(comma + 1);
        if (*ptrtok == '\0' || *sizetok == '\0') fail("'libc-heap-realloc' requires both a pointer and a size argument");

        char *gt = strchr(close + 1, '>');
        if (!gt) fail("'libc-heap-realloc' expects 'libc-heap-realloc(rPTR, N) > rX' syntax");
        char *dsttok = trim(gt + 1);
        strip__semicolon(dsttok);
        if (*dsttok == '\0') fail("'libc-heap-realloc' expects a destination register");

        operand_t dstop;
        parse__operand(dsttok, &dstop);
        if (dstop.kind != OPND_REG || dstop.is_sp) fail("'libc-heap-realloc' requires a register as its destination (libc-heap-realloc(rPTR, N) > rX)");

        ensure_libc_heap_extern("realloc", 2);

        instr_t c; memset(&c, 0, sizeof(c));
        c.op = OP_LIBC_CALL;
        strncpy(c.dst.sym, "realloc", MAX_SYMLEN - 1);
        parse__operand(ptrtok, &c.args[0]);
        parse__operand(sizetok, &c.args[1]);
        c.nargs = 2;
        push__instr(c);

        instr_t *libc_ins = &prog[nprog - 1];
        libc_ins->dst.kind = OPND_REG;
        libc_ins->dst.reg_num = dstop.reg_num;
        return 1;
    }

    /* loadN [ADDR] > rDST; / storeN rSRC > [ADDR];  -- sized access to
       an absolute, compile-time-constant memory address (see
       parse__operand's OPND_ADDR / '[EXPR]' handling). Unlike plain
       'load SYM > rX;'/'store rX > SYM;' (whose width comes from the
       symbol's own decls[] entry), a bare numeric address has no
       declaration to size itself from, so loadN/storeN's N suffix is
       how the width gets specified -- same N-in-{8,16,32,64} convention
       as iloadN/istoreN, so nothing new to learn if you already know
       that family. This is the loadN/storeN case of the OP_LOAD/
       OP_STORE opcodes (not a new opcode of its own): parse__operand
       already leaves an OPND_ADDR's local_size at 0 (unsized), and this
       block's only job is to stamp the real width in from the suffix
       before falling through to plain OP_LOAD/OP_STORE, so every
       backend's existing OP_LOAD/OP_STORE codegen (which already reads
       operand_mem_size(&op)) needs only one small addition -- an
       OPND_ADDR case in operand_mem_size and an OPND_ADDR case in each
       backend's _addr_text-equivalent helper -- to support it, rather
       than a parallel code path.
       Only fires for loadN/storeN; plain unsuffixed 'load'/'store' on a
       symbol or local are completely unaffected and keep working
       exactly as before.

       Sign vs zero extend: loadN comes in two spellings -- 'loadN'
       (zero-extend, e.g. 'load8', 'load32') and 'loadNs' (sign-extend,
       e.g. 'load8s', 'load32s') for every width below 64 (64-bit fills
       the whole register regardless, so there's nothing to extend and
       no 's' form exists for it). storeN has no such split: truncating
       a 64-bit register down to N bits on the way out never needs to
       decide how those bits got there, so store8/16/32/64 are
       unaffected by any of this. This mirrors the real machine: x86-64
       has 'movzx'/'movsx', AArch64 has plain-vs-'s' load mnemonics
       (ldrb/ldrsb etc), RISC-V has 'lbu'/'lb' -- Chard used to pick one
       of those two behaviors for you per-width per-arch (and, before
       this, inconsistently: RISC-V's plain loadN already sign-extended
       while x86-64/AArch64's zero-extended -- the same Chard source
       silently meant two different things depending which -target flag
       compiled it). Now the choice is explicit in the mnemonic and the
       same on every target. */
    if ((strncmp(tokens[0], "load", 4) == 0 && isdigit((unsigned char)tokens[0][4])) ||
        (strncmp(tokens[0], "store", 5) == 0 && isdigit((unsigned char)tokens[0][5]))) {
        int is_load = (strncmp(tokens[0], "load", 4) == 0);
        int suffix_off = is_load ? 4 : 5;
        /* Walk the digit run explicitly rather than trusting atoi's
           stop-at-first-non-digit behavior to also validate the input:
           atoi("8x") == 8 silently, which would let a typo like
           'load8x' compile as if it were plain 'load8' with the 'x'
           just ignored -- exactly the kind of silent misparse this
           whole explicit-suffix feature exists to avoid. So the digits
           are consumed by hand, and everything after them is checked
           to be either nothing (plain load8/16/32/64) or exactly one
           's' character and nothing more (load8s/16s/32s) -- any other
           trailing text is a hard error naming the bad suffix. */
        const char *p = tokens[0] + suffix_off;
        const char *digits_start = p;
        while (isdigit((unsigned char)*p)) p++;
        int bits = atoi(digits_start);
        int wants_signed = 0;
        if (*p == 's' && *(p + 1) == '\0') {
            wants_signed = 1;
        } else if (*p != '\0') {
            char msg[128];
            snprintf(msg, sizeof(msg), "'%s': unrecognized suffix -- use %s8/16/32/64%s, nothing else",
                     tokens[0], is_load ? "load" : "store", is_load ? " or ...s (sign-extend)" : "");
            fail(msg);
        }
        if (wants_signed && !is_load) {
            char msg[96];
            snprintf(msg, sizeof(msg), "'%s': store has no signed/unsigned distinction -- use plain 'store%d', not 'store%ds'", tokens[0], bits, bits);
            fail(msg);
        }
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8) {
            char msg[96];
            snprintf(msg, sizeof(msg), "'%s' has invalid width: use %s8/16/32/64%s", tokens[0], is_load ? "load" : "store", is_load ? " (append 's' for sign-extend, e.g. load32s)" : "");
            fail(msg);
        }
        if (wants_signed && esz == 8) {
            char msg[160];
            snprintf(msg, sizeof(msg), "'%s': a 64-bit load fills the whole register -- there's no extension to sign, use plain 'load64'", tokens[0]);
            fail(msg);
        }
        if (ntok < 4 || strcmp(tokens[2], ">") != 0)
            failf("'%s' expects 'src > dst' operand syntax", tokens[0]);
        char dsttok[MAX_SYMLEN];
        strncpy(dsttok, tokens[3], MAX_SYMLEN - 1);
        dsttok[MAX_SYMLEN - 1] = '\0';
        strip__semicolon(dsttok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = is_load ? OP_LOAD : OP_STORE;
        i.load_signed = wants_signed;
        parse__operand(tokens[1], &i.src);
        parse__operand(dsttok, &i.dst);

        if (is_load) {
            if (i.src.kind != OPND_ADDR) failf("'%s' (a sized load) requires '[ADDR]' as its source -- use plain 'load' for a symbol or local", tokens[0]);
            if (i.dst.kind != OPND_REG) failf("'%s' requires a register as its destination", tokens[0]);
            i.src.local_size = esz;
        } else {
            if (i.src.kind != OPND_REG) failf("'%s' requires a register as its source", tokens[0]);
            if (i.dst.kind != OPND_ADDR) failf("'%s' (a sized store) requires '[ADDR]' as its destination -- use plain 'store' for a symbol or local", tokens[0]);
            i.dst.local_size = esz;
        }
        push__instr(i);
        return 1;
    }

    /* sextN SRC > rDST; / zextN SRC > rDST; -- sign/zero-extend a value
       already held in a register or immediate, filling the rest of the
       64-bit destination register. N is 8/16/32 (no N64: a 64-bit
       value already fills the register, there's nothing to extend --
       same "no 's' form at the full width" reasoning iloadN's own
       suffix already follows).

       This is deliberately NOT the same job iloadNs already does:
       iloadNs sign-extends a narrower value on its way IN from memory.
       sext/zext operate on a value that's already sitting in a
       register (e.g. the result of a narrower computation, or a value
       that arrived via a call), where there was never a load to attach
       a suffix to. SRC is register or immediate only -- not a bare
       memory operand -- for the same reason: if the value lives in
       memory, iloadNs/loadNs already own that job, and giving sext/zext
       a second, redundant way to do it would just be two spellings for
       one thing. */
    if ((strncmp(tokens[0], "sext", 4) == 0 || strncmp(tokens[0], "zext", 4) == 0) &&
        isdigit((unsigned char)tokens[0][4])) {
        int is_signed = tokens[0][0] == 's';
        const char *p = tokens[0] + 4;
        const char *digits_start = p;
        while (isdigit((unsigned char)*p)) p++;
        if (*p != '\0')
            fail_fmt("'%s': unrecognized suffix -- use %sext8/16/32, nothing else", tokens[0], is_signed ? "s" : "z");
        int bits = atoi(digits_start);
        int esz = bits / 8;
        if ((bits % 8) != 0 || (esz != 1 && esz != 2 && esz != 4))
            fail_fmt("'%s' has invalid width: use %sext8/16/32 -- a 64-bit value already fills the register, there's nothing to extend", tokens[0], is_signed ? "s" : "z");

        if (!(ntok >= 4 && strcmp(tokens[2], ">") == 0))
            failf("'%s' expects 'src > dst' operand syntax", tokens[0]);
        char lhstok[MAX_SYMLEN], rhstok[MAX_SYMLEN];
        strncpy(lhstok, tokens[1], MAX_SYMLEN - 1); lhstok[MAX_SYMLEN - 1] = '\0';
        strncpy(rhstok, tokens[3], MAX_SYMLEN - 1); rhstok[MAX_SYMLEN - 1] = '\0';
        strip__semicolon(rhstok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = is_signed ? OP_SEXT : OP_ZEXT;
        i.elem_size = esz;
        parse__operand(lhstok, &i.src);
        parse__operand(rhstok, &i.dst);
        if (i.src.kind != OPND_REG && i.src.kind != OPND_IMM)
            fail_fmt("'%s' requires a register or immediate as its source -- for a value in memory, use %s instead",
                  tokens[0], is_signed ? "iloadNs (or loadNs)" : "iloadN (or loadN)");
        if (i.dst.kind != OPND_REG)
            failf("'%s' requires a register as its destination", tokens[0]);
        push__instr(i);
        return 1;
    }

    /* iloadN rBASE[rIDX] > rDST; -- indexed heap load. rBASE is a
       pointer previously obtained from alloc()/lea/another load; rIDX
       is an element index (not a byte offset -- it's scaled by N/8
       internally, same as a C array subscript). N is 8/16/32/64,
       matching the iN size suffix used everywhere else in Chard.
       There's no bounds checking: heap arrays are raw memory, same
       philosophy as alloc() having no free().

       Sign vs zero extend: 'iloadNs' (e.g. 'iload8s') sign-extends
       into the destination register; plain 'iloadN' zero-extends, as
       it always has. No 's' form for iload64 -- same reasoning as
       loadN's 64-bit case: the whole register is filled either way,
       there's no extension to have an opinion about. See the loadN/
       storeN block above for the full rationale (this is the same
       split, just for the indexed-heap-load opcode instead of the
       symbol/absolute-address one). istoreN has no signed variant for
       the same reason storeN doesn't: truncating on the way out never
       needs to know how the bits arrived. */
    if (strncmp(tokens[0], "iload", 5) == 0 && isdigit((unsigned char)tokens[0][5])) {
        /* Walk the digit run explicitly and validate the remainder is
           either empty or exactly 's' -- same reasoning as loadN/
           storeN above: atoi("8x") == 8 would otherwise silently
           accept a typo'd 'iload8x' as if it were plain 'iload8',
           quietly discarding the 'x' instead of erroring on it. */
        const char *p = tokens[0] + 5;
        const char *digits_start = p;
        while (isdigit((unsigned char)*p)) p++;
        int bits = atoi(digits_start);
        int wants_signed = 0;
        if (*p == 's' && *(p + 1) == '\0') {
            wants_signed = 1;
        } else if (*p != '\0') {
            failf("'%s': unrecognized suffix -- use iload8/16/32/64 or ...s (sign-extend), nothing else", tokens[0]);
        }
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8)
            failf("'%s' has invalid width: use iload8/16/32/64 (append 's' for sign-extend, e.g. iload32s)", tokens[0]);
        if (wants_signed && esz == 8)
            failf("'%s': a 64-bit load fills the whole register -- there's no extension to sign, use plain 'iload64'", tokens[0]);

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char basetok[MAX_SYMLEN], idxtok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
        int consumed = 0;
        if (sscanf(buf, "%*s %63[^[][ %63[^]] ] > %63s %n", basetok, idxtok, dsttok, &consumed) != 3)
            fail("malformed 'iloadN': expected 'iloadN rBASE[rIDX] > rDST;'");
        strip__semicolon(dsttok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_ILOAD;
        i.elem_size = esz;
        i.load_signed = wants_signed;
        if (!parse__register(basetok, &i.base_reg)) failf("iloadN: '%s' is not a valid register", basetok);
        if (!parse__register(idxtok, &i.idx_reg)) failf("iloadN: '%s' is not a valid register", idxtok);
        parse__operand(dsttok, &i.dst);
        if (i.dst.kind != OPND_REG) fail("iloadN: destination must be a register");
        push__instr(i);
        return 1;
    }

    /* istoreN rSRC > rBASE[rIDX]; -- indexed heap store, mirrors
       iloadN. Note the operand order matches Chard's usual 'src > dst'
       rule: the value being written comes first, the array slot (the
       destination) comes after '>'. */
    if (strncmp(tokens[0], "istore", 6) == 0 && isdigit((unsigned char)tokens[0][6])) {
        /* Same digit-run-then-validate-remainder approach as iloadN/
           loadN/storeN above -- istoreN has no sign suffix at all (see
           the block comment above this one: truncating on the way out
           never needs to know how the bits arrived), so the only valid
           remainder after the digits is empty; anything else, 's'
           included, is a hard error rather than a silently-ignored
           typo. */
        const char *p = tokens[0] + 6;
        const char *digits_start = p;
        while (isdigit((unsigned char)*p)) p++;
        int bits = atoi(digits_start);
        if (*p != '\0') {
            char msg[128];
            if (*p == 's' && *(p + 1) == '\0')
                snprintf(msg, sizeof(msg), "'%s': istore has no signed/unsigned distinction -- use plain 'istore%d', not 'istore%ds'", tokens[0], bits, bits);
            else
                snprintf(msg, sizeof(msg), "'%s': unrecognized suffix -- use istore8/16/32/64, nothing else", tokens[0]);
            fail(msg);
        }
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8)
            failf("'%s' has invalid width: use istore8/16/32/64", tokens[0]);

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char srctok[MAX_SYMLEN], basetok[MAX_SYMLEN], idxtok[MAX_SYMLEN];
        int consumed = 0;
        if (sscanf(buf, "%*s %63s > %63[^[][ %63[^]] ] %n", srctok, basetok, idxtok, &consumed) != 3)
            fail("malformed 'istoreN': expected 'istoreN rSRC > rBASE[rIDX];'");
        char *close_check = strchr(buf, ']');
        if (!close_check) fail("malformed 'istoreN': missing ']'");

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_ISTORE;
        i.elem_size = esz;
        parse__operand(srctok, &i.src);
        if (!parse__register(basetok, &i.base_reg)) failf("istoreN: '%s' is not a valid register", basetok);
        if (!parse__register(idxtok, &i.idx_reg)) failf("istoreN: '%s' is not a valid register", idxtok);
        push__instr(i);
        return 1;
    }

    /* hfieldN rBASE.field > rDST;  /  hfieldN rSRC > rBASE.field;
       -- struct field access through a heap pointer register (from
       alloc()/lea/another load). Unlike sfieldN (a compile-time stack
       offset, desugared into the existing OP_LALOAD/OP_LASTORE), a
       heap struct's base address is only known at runtime, so this is
       a real new opcode: base register + a compile-time constant byte
       offset, unscaled -- see OP_HFIELD_LOAD's comment on opcode_t for
       why that's a different addressing mode than OP_ILOAD's runtime-
       scaled index, and thus deliberately not the same opcode. */
    if (strncmp(tokens[0], "hfield", 6) == 0 && isdigit((unsigned char)tokens[0][6])) {
        int bits = atoi(tokens[0] + 6);
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8)
            failf("'%s' has invalid width: use hfield8/16/32/64", tokens[0]);

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        strip__semicolon(buf);
        char *gt = strchr(buf, '>');
        if (!gt) fail("malformed 'hfieldN': expected 'hfieldN rBASE.field > rDST;' or 'hfieldN rSRC > rBASE.field;'");
        char lhs[MAX_LINE], rhs[MAX_LINE];
        size_t lhslen = (size_t)(gt - buf);
        if (lhslen >= sizeof(lhs)) lhslen = sizeof(lhs) - 1;
        memcpy(lhs, buf, lhslen); lhs[lhslen] = '\0';
        strncpy(rhs, gt + 1, sizeof(rhs) - 1); rhs[sizeof(rhs) - 1] = '\0';
        char *lhs_after_mnemonic = lhs + strlen(tokens[0]);
        char *lhstrim = trim(lhs_after_mnemonic);
        char *rhstrim = trim(rhs);

        char *dot_l = strchr(lhstrim, '.');
        char *dot_r = strchr(rhstrim, '.');
        if ((dot_l != NULL) == (dot_r != NULL))
            fail("malformed 'hfieldN': exactly one side must be 'rBASE.field' (the other a register)");

        int is_load = (dot_l != NULL);
        char *dotted = is_load ? lhstrim : rhstrim;
        char *regtok = is_load ? rhstrim : lhstrim;

        char *dot = strchr(dotted, '.');
        *dot = '\0';
        char *basetok = trim(dotted);
        char *fieldname = trim(dot + 1);

        /* hfieldN's struct type isn't spelled anywhere in the syntax
           itself -- rBASE is just a plain pointer register, with no
           static type of its own in Chard (unlike a 'local StructName
           name;' instance, which does carry its type on the decl).
           So the field name alone must uniquely identify which struct
           it belongs to: if more than one struct defines a field with
           this name, hfieldN can't tell which layout to use and this
           is a hard error asking for sfieldN-style disambiguation
           instead (there isn't one yet in v1 -- see the note below). */
        struct_def_t *owner = NULL;
        int ambiguous = 0;
        for (int si = 0; si < nstruct_defs; si++) {
            if (find_struct_field(&struct_defs[si], fieldname)) {
                if (owner) { ambiguous = 1; break; }
                owner = &struct_defs[si];
            }
        }
        if (!owner) {
            char msg[256];
            snprintf(msg, sizeof(msg), "hfieldN: no struct has a field named '%s'", fieldname);
            fail(msg);
        }
        if (ambiguous) {
            char msg[256];
            snprintf(msg, sizeof(msg), "hfieldN: field '%s' is ambiguous (more than one struct defines it) -- give it a unique name", fieldname);
            fail(msg);
        }
        struct_field_t *fld = find_struct_field(owner, fieldname);
        if (esz != fld->size_bytes) {
            char msg[256];
            snprintf(msg, sizeof(msg), "hfieldN: width doesn't match field '%s.%s' (declared %d bytes)", owner->name, fieldname, fld->size_bytes);
            fail(msg);
        }

        operand_t baseop;
        if (!parse__register(basetok, &baseop)) failf("hfieldN: '%s' is not a valid base register", basetok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.elem_size = esz;
        i.base_reg = baseop;
        i.const_offset = fld->offset;
        if (is_load) {
            i.op = OP_HFIELD_LOAD;
            parse__operand(regtok, &i.dst);
            if (i.dst.kind != OPND_REG) fail("hfieldN: destination must be a register");
        } else {
            i.op = OP_HFIELD_STORE;
            parse__operand(regtok, &i.src);
        }
        push__instr(i);
        return 1;
    }

    /* xloadN rBASE[rIDX*SCALE+DISP] > rDST;
       xstoreN rSRC > rBASE[rIDX*SCALE+DISP];
       -- general base+index*scale+displacement heap addressing: the
       union of what OP_ILOAD (scale fixed to elem_size, no
       displacement) and OP_HFIELD_LOAD (displacement only, no index)
       each offer separately. '*SCALE' and '+DISP'/'-DISP' are each
       independently optional: 'rBASE[rIDX]' (scale defaults to 1, disp
       to 0), 'rBASE[rIDX*4]', 'rBASE[rIDX+8]', 'rBASE[rIDX*4-8]' are
       all valid, parsed by parse__xaddr below. SCALE must be a
       compile-time-constant 1/2/4/8 -- see OP_XLOAD's comment on
       opcode_t for why an arbitrary multiplier isn't accepted. */
    if ((strncmp(tokens[0], "xload", 5) == 0 && isdigit((unsigned char)tokens[0][5])) ||
        (strncmp(tokens[0], "xstore", 6) == 0 && isdigit((unsigned char)tokens[0][6]))) {
        int is_load = (tokens[0][1] == 'l');
        int bits = atoi(tokens[0] + (is_load ? 5 : 6));
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8) {
            char msg[128];
            snprintf(msg, sizeof(msg), "'%s' has invalid width: use %s8/16/32/64", tokens[0], is_load ? "xload" : "xstore");
            fail(msg);
        }

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        strip__semicolon(buf);

        char basetok[MAX_SYMLEN], addrtok[MAX_LINE], othertok[MAX_SYMLEN];
        if (is_load) {
            /* xloadN rBASE[EXPR] > rDST; */
            if (sscanf(buf, "%*s %63[^[][ %255[^]] ] > %63s", basetok, addrtok, othertok) != 3)
                fail("malformed 'xloadN': expected 'xloadN rBASE[rIDX*SCALE+DISP] > rDST;'");
        } else {
            /* xstoreN rSRC > rBASE[EXPR]; */
            if (sscanf(buf, "%*s %63s > %63[^[][ %255[^]] ]", othertok, basetok, addrtok) != 3)
                fail("malformed 'xstoreN': expected 'xstoreN rSRC > rBASE[rIDX*SCALE+DISP];'");
            if (!strchr(buf, ']')) fail("malformed 'xstoreN': missing ']'");
        }

        operand_t baseop, idxop;
        if (!parse__register(trim(basetok), &baseop)) {
            char msg[192];
            snprintf(msg, sizeof(msg), "%s: '%s' is not a valid base register", tokens[0], basetok);
            fail(msg);
        }

        int scale = 1;
        long disp = 0;
        parse__xaddr(tokens[0], addrtok, &idxop, &scale, &disp);
        if (idxop.kind != OPND_REG || idxop.is_float) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s: index must be an integer register", tokens[0]);
            fail(msg);
        }

        instr_t i; memset(&i, 0, sizeof(i));
        i.base_reg = baseop;
        i.idx_reg = idxop;
        i.elem_size = esz;
        i.xaddr_scale = scale;
        i.const_offset = (int)disp;
        if (disp != (long)i.const_offset) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s: displacement %ld is out of range", tokens[0], disp);
            fail(msg);
        }
        if (is_load) {
            i.op = OP_XLOAD;
            parse__operand(othertok, &i.dst);
            if (i.dst.kind != OPND_REG) {
                char msg[128];
                snprintf(msg, sizeof(msg), "%s: destination must be a register", tokens[0]);
                fail(msg);
            }
        } else {
            i.op = OP_XSTORE;
            parse__operand(othertok, &i.src);
        }
        push__instr(i);
        return 1;
    }

    /* ptradd rBASE[rIDX*SCALE+DISP] > rDST;   rDST = rBASE + rIDX*SCALE + DISP
       ptradd rBASE + DISP > rDST;             rDST = rBASE + DISP (no index)
       ptrsub rBASE[rIDX*SCALE+DISP] > rDST;   rDST = rBASE - (rIDX*SCALE + DISP)
       ptrsub rBASE - DISP > rDST;             rDST = rBASE - DISP (no index)
       -- pointer arithmetic: computes an address into rDST without
       dereferencing it (see OP_PTRADD's comment on opcode_t). The
       bracket form reuses parse__xaddr, exactly like xloadN/xstoreN
       above, just producing the address itself as the result instead
       of loading/storing through it. The bracket-less form is a
       convenience for the common "just add a byte offset" case, where
       spelling a whole '[r1*1+8]' index expression for a fixed
       constant would be needless ceremony -- 'ptradd rBASE + 8 >
       rDST;' says the same thing plainly. Exactly one of the two forms
       must be used per instruction (detected by whether tokens[0]'s
       raw text contains '['), not both at once. */
    if (strcmp(tokens[0], "ptradd") == 0 || strcmp(tokens[0], "ptrsub") == 0) {
        int is_sub = (tokens[0][3] == 's');
        char pmsg[256];

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        strip__semicolon(buf);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = is_sub ? OP_PTRSUB : OP_PTRADD;

        if (strchr(buf, '[')) {
            /* Bracket form: identical grammar to xloadN's load side,
               'MNEMONIC rBASE[EXPR] > rDST;'. */
            char basetok[MAX_SYMLEN], addrtok[MAX_LINE], dsttok[MAX_SYMLEN];
            if (sscanf(buf, "%*s %63[^[][ %255[^]] ] > %63s", basetok, addrtok, dsttok) != 3) {
                snprintf(pmsg, sizeof(pmsg), "malformed '%s': expected '%s rBASE[rIDX*SCALE+DISP] > rDST;'", tokens[0], tokens[0]);
                fail(pmsg);
            }

            operand_t baseop, idxop;
            if (!parse__register(trim(basetok), &baseop)) {
                snprintf(pmsg, sizeof(pmsg), "%s: '%s' is not a valid base register", tokens[0], basetok);
                fail(pmsg);
            }

            int scale = 1;
            long disp = 0;
            parse__xaddr(tokens[0], addrtok, &idxop, &scale, &disp);
            if (idxop.kind != OPND_REG || idxop.is_float) {
                snprintf(pmsg, sizeof(pmsg), "%s: index must be an integer register", tokens[0]);
                fail(pmsg);
            }

            i.base_reg = baseop;
            i.idx_reg = idxop;
            i.xaddr_scale = scale;
            i.const_offset = (int)disp;
            if (disp != (long)i.const_offset) {
                snprintf(pmsg, sizeof(pmsg), "%s: displacement %ld is out of range", tokens[0], disp);
                fail(pmsg);
            }

            parse__operand(dsttok, &i.dst);
            if (i.dst.kind != OPND_REG) {
                snprintf(pmsg, sizeof(pmsg), "%s: destination must be a register", tokens[0]);
                fail(pmsg);
            }
        } else {
            /* Bracket-less form: 'MNEMONIC rBASE + DISP > rDST;' (the
               sign here is purely which mnemonic was used -- ptradd
               always adds DISP, ptrsub always subtracts it, so only a
               plain '+' is accepted here; writing 'ptradd rBASE - 8'
               should use 'ptrsub rBASE + 8' instead, matching how
               Chard elsewhere avoids offering two spellings of the
               same operation). idx_reg is left at its zeroed
               OPND_NONE kind, which codegen (see each backend's
               OP_PTRADD/OP_PTRSUB case) reads as "no index term, just
               base+disp". */
            char basetok[MAX_SYMLEN], disptok[64], dsttok[MAX_SYMLEN];
            if (sscanf(buf, "%*s %63[^+] + %63[^>] > %63s", basetok, disptok, dsttok) != 3) {
                snprintf(pmsg, sizeof(pmsg), "malformed '%s': expected '%s rBASE + DISP > rDST;'", tokens[0], tokens[0]);
                fail(pmsg);
            }

            operand_t baseop;
            if (!parse__register(trim(basetok), &baseop)) {
                snprintf(pmsg, sizeof(pmsg), "%s: '%s' is not a valid base register", tokens[0], basetok);
                fail(pmsg);
            }

            char *dt = trim(disptok);
            if (!is__number(dt)) {
                long ev;
                if (!try_parse_paren_expr(dt, &ev)) {
                    snprintf(pmsg, sizeof(pmsg), "%s: '%s' is not a valid displacement (must be a compile-time integer)", tokens[0], dt);
                    fail(pmsg);
                }
                i.const_offset = (int)ev;
                if (ev != (long)i.const_offset) {
                    snprintf(pmsg, sizeof(pmsg), "%s: displacement %ld is out of range", tokens[0], ev);
                    fail(pmsg);
                }
            } else {
                long disp = parse__number(dt);
                i.const_offset = (int)disp;
                if (disp != (long)i.const_offset) {
                    snprintf(pmsg, sizeof(pmsg), "%s: displacement %ld is out of range", tokens[0], disp);
                    fail(pmsg);
                }
            }

            i.base_reg = baseop;
            i.idx_reg.kind = OPND_NONE;
            i.xaddr_scale = 1;

            parse__operand(dsttok, &i.dst);
            if (i.dst.kind != OPND_REG) {
                snprintf(pmsg, sizeof(pmsg), "%s: destination must be a register", tokens[0]);
                fail(pmsg);
            }
        }

        push__instr(i);
        return 1;
    }

    /* sfieldN name.field > rDST;  /  sfieldN rSRC > name.field;
       -- struct field access on a stack ('local StructName name;')
       instance. Not a new opcode: this hand-builds the exact same
       OP_LALOAD/OP_LASTORE instr_t laloadN/lastoreN already produce
       (see immediately above), with idx_reg fixed at the literal 0 and
       the field's byte offset folded directly into local_offset --
       'struct base_offset - field->offset' is exactly what codegen's
       existing 'local_offset - idx*elem_size' address formula needs to
       land on the field's own address when idx is 0. Reusing the
       opcode this way means every backend's OP_LALOAD/OP_LASTORE case
       already knows how to emit this correctly; there is deliberately
       no separate codegen path to keep in sync. N (8/16/32/64) must
       match the field's own declared width -- same discipline
       laloadN/lastoreN already enforce against an array's declared
       element size, just checked against the one field instead. */
    if (strncmp(tokens[0], "sfield", 6) == 0 && isdigit((unsigned char)tokens[0][6])) {
        int bits = atoi(tokens[0] + 6);
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8)
            failf("'%s' has invalid width: use sfield8/16/32/64", tokens[0]);

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        strip__semicolon(buf);
        char *gt = strchr(buf, '>');
        if (!gt) fail("malformed 'sfieldN': expected 'sfieldN name.field > rDST;' or 'sfieldN rSRC > name.field;'");
        char lhs[MAX_LINE], rhs[MAX_LINE];
        size_t lhslen = (size_t)(gt - buf);
        if (lhslen >= sizeof(lhs)) lhslen = sizeof(lhs) - 1;
        memcpy(lhs, buf, lhslen); lhs[lhslen] = '\0';
        strncpy(rhs, gt + 1, sizeof(rhs) - 1); rhs[sizeof(rhs) - 1] = '\0';
        /* lhs still has the leading 'sfieldN' mnemonic token in it -- strip it */
        char *lhs_after_mnemonic = lhs + strlen(tokens[0]);
        char *lhstrim = trim(lhs_after_mnemonic);
        char *rhstrim = trim(rhs);

        char *dot_l = strchr(lhstrim, '.');
        char *dot_r = strchr(rhstrim, '.');
        if ((dot_l != NULL) == (dot_r != NULL))
            fail("malformed 'sfieldN': exactly one side must be 'name.field' (the other a register)");

        int is_load = (dot_l != NULL); /* dotted side is on the left -> load form */
        char *dotted = is_load ? lhstrim : rhstrim;
        char *regtok = is_load ? rhstrim : lhstrim;

        char *dot = strchr(dotted, '.');
        *dot = '\0';
        char *instname = trim(dotted);
        char *fieldname = trim(dot + 1);

        decl_t *sdecl = find__decl(instname);
        if (!sdecl || sdecl->section != SEC_LOCAL || sdecl->struct_type_name[0] == '\0')
            failf("sfieldN: '%s' is not an in-scope local struct instance", instname);
        struct_def_t *sd = find_struct_def(sdecl->struct_type_name);
        if (!sd) failf("sfieldN: internal error resolving struct type for '%s'", instname); /* unreachable: type existed when the local was declared */
        struct_field_t *fld = find_struct_field(sd, fieldname);
        if (!fld) {
            char msg[256];
            snprintf(msg, sizeof(msg), "sfieldN: struct '%s' has no field '%s'", sd->name, fieldname);
            fail(msg);
        }
        if (esz != fld->size_bytes) {
            char msg[256];
            snprintf(msg, sizeof(msg), "sfieldN: width doesn't match field '%s.%s' (declared %d bytes)", sd->name, fieldname, fld->size_bytes);
            fail(msg);
        }

        operand_t instop;
        parse__operand(instname, &instop); /* resolves base offset/frames_up for the struct instance */
        instop.local_offset -= fld->offset; /* base_offset - field_offset: see comment above */
        instop.local_size = esz;

        operand_t idx_zero; memset(&idx_zero, 0, sizeof(idx_zero));
        idx_zero.kind = OPND_IMM;
        idx_zero.imm = 0;

        instr_t i; memset(&i, 0, sizeof(i));
        i.elem_size = esz;
        i.idx_reg = idx_zero;
        if (is_load) {
            i.op = OP_LALOAD;
            i.src = instop;
            parse__operand(regtok, &i.dst);
            if (i.dst.kind != OPND_REG) fail("sfieldN: destination must be a register");
        } else {
            i.op = OP_LASTORE;
            i.dst = instop;
            parse__operand(regtok, &i.src);
        }
        push__instr(i);
        return 1;
    }

    /* laloadN name[rIDX] > rDST; -- indexed local-array load. Mirrors
       iloadN's syntax exactly, except the base is a local array's name
       (a compile-time-known fp-relative address -- see
       declare_local_array) rather than a runtime register holding a
       heap pointer. N must match the array's own declared element
       width; a literal index is bounds-checked against the array's
       declared length right here at parse time (the one place Chard
       can catch it for free), since laloadN/lastoreN have no runtime
       bounds checking otherwise -- matching iloadN/istoreN's existing
       "no checks" precedent for anything it can't verify statically. */
    if (strncmp(tokens[0], "laload", 6) == 0 && isdigit((unsigned char)tokens[0][6])) {
        int bits = atoi(tokens[0] + 6);
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8)
            failf("'%s' has invalid width: use laload8/16/32/64", tokens[0]);

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char nametok[MAX_SYMLEN], idxtok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63[^[][ %63[^]] ] > %63s", nametok, idxtok, dsttok) != 3)
            fail("malformed 'laloadN': expected 'laloadN name[rIDX] > rDST;'");
        strip__semicolon(dsttok);

        decl_t *arrd = find__decl(nametok);
        if (!arrd || arrd->section != SEC_LOCAL) failf("laloadN: '%s' is not an in-scope local array", nametok);
        if (arrd->array_len == 0) failf("laloadN: '%s' is a scalar local, not an array (use load/store instead)", nametok);
        if (esz != arrd->size_bytes) failf("laloadN: width doesn't match how '%s' was declared", nametok);

        operand_t arrop;
        parse__operand(nametok, &arrop); /* resolves offset/frames_up for element 0 */

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_LALOAD;
        i.elem_size = esz;
        i.src = arrop;
        if (is__number(idxtok)) {
            long lit = parse__number(idxtok);
            if (lit < 0 || lit >= arrd->array_len)
                failf("laloadN: literal index out of bounds for '%s'", nametok);
        }
        if (!parse__register(idxtok, &i.idx_reg)) {
            /* a literal index is also valid -- reuse parse__operand and
               require it resolve to an immediate, so 'laload8 buf[3]'
               works without forcing a register just to hold a constant */
            operand_t idxop;
            parse__operand(idxtok, &idxop);
            if (idxop.kind != OPND_IMM) failf("laloadN: '%s' is not a valid register or integer index", idxtok);
            i.idx_reg = idxop;
        }
        parse__operand(dsttok, &i.dst);
        if (i.dst.kind != OPND_REG) fail("laloadN: destination must be a register");
        push__instr(i);
        return 1;
    }

    /* lastoreN rSRC > name[rIDX]; -- indexed local-array store, mirrors
       laloadN. Operand order matches Chard's usual 'src > dst' rule. */
    if (strncmp(tokens[0], "lastore", 7) == 0 && isdigit((unsigned char)tokens[0][7])) {
        int bits = atoi(tokens[0] + 7);
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8)
            failf("'%s' has invalid width: use lastore8/16/32/64", tokens[0]);

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char srctok[MAX_SYMLEN], nametok[MAX_SYMLEN], idxtok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63s > %63[^[][ %63[^]] ] ", srctok, nametok, idxtok) != 3)
            fail("malformed 'lastoreN': expected 'lastoreN rSRC > name[rIDX];'");
        char *close_check2 = strchr(buf, ']');
        if (!close_check2) fail("malformed 'lastoreN': missing ']'");

        decl_t *arrd = find__decl(nametok);
        if (!arrd || arrd->section != SEC_LOCAL) failf("lastoreN: '%s' is not an in-scope local array", nametok);
        if (arrd->array_len == 0) failf("lastoreN: '%s' is a scalar local, not an array (use load/store instead)", nametok);
        if (esz != arrd->size_bytes) failf("lastoreN: width doesn't match how '%s' was declared", nametok);

        operand_t arrop;
        parse__operand(nametok, &arrop);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_LASTORE;
        i.elem_size = esz;
        i.dst = arrop;
        parse__operand(srctok, &i.src);
        if (is__number(idxtok)) {
            long lit = parse__number(idxtok);
            if (lit < 0 || lit >= arrd->array_len)
                failf("lastoreN: literal index out of bounds for '%s'", nametok);
        }
        if (!parse__register(idxtok, &i.idx_reg)) {
            operand_t idxop;
            parse__operand(idxtok, &idxop);
            if (idxop.kind != OPND_IMM) failf("lastoreN: '%s' is not a valid register or integer index", idxtok);
            i.idx_reg = idxop;
        }
        push__instr(i);
        return 1;
    }

    /* Emits a runtime bounds check for claloadN/clastoreN (see below):
     * traps (same OP_ASSERT/'ud2' mechanism 'assert' already uses) if
     * idx_reg >= len, unsigned -- so a negative index (read as a huge
     * unsigned value) traps exactly like an out-of-range positive one,
     * rather than wrapping around and passing a signed check. Only
     * called when the index is a register: a literal index is already
     * checked once, for free, at parse time (same as laloadN/lastoreN
     * -- see either's own comment), so emitting a redundant runtime
     * check for that case would just be dead weight in the output. */
    /* claloadN name[rIDX] > rDST; / clastoreN rSRC > name[rIDX]; --
       bounds-CHECKED siblings of laloadN/lastoreN: identical syntax and
       identical semantics on success, but a register index gets a real
       runtime check (trap via 'ud2' on out-of-range, same as 'assert')
       instead of laloadN/lastoreN's documented "no runtime bounds
       checking, same as raw memory" behavior. This is opt-in, not a
       replacement -- laloadN/lastoreN stay exactly as unchecked as
       they always were, the same explicit-tradeoff philosophy
       spill/unspill and the rest of Chard already follows (nothing
       here is checked by default; checking is something the
       programmer reaches for on purpose, at the call site, when they
       want it). The bulk of the parsing is identical to laloadN/
       lastoreN by design -- same grammar, same errors for a malformed
       line, same width/array/scalar validation -- so this is written
       as a thin wrapper: parse exactly the way laloadN/lastoreN do,
       just emit an extra bounds check first when the index is a
       register (a literal index is already checked once, for free, at
       parse time -- same as laloadN/lastoreN, so no redundant runtime
       check is emitted for that case). */
    if (strncmp(tokens[0], "claload", 7) == 0 && isdigit((unsigned char)tokens[0][7])) {
        int bits = atoi(tokens[0] + 7);
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8)
            failf("'%s' has invalid width: use claload8/16/32/64", tokens[0]);

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char nametok[MAX_SYMLEN], idxtok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63[^[][ %63[^]] ] > %63s", nametok, idxtok, dsttok) != 3)
            fail("malformed 'claloadN': expected 'claloadN name[rIDX] > rDST;'");
        strip__semicolon(dsttok);

        decl_t *arrd = find__decl(nametok);
        if (!arrd || arrd->section != SEC_LOCAL) failf("claloadN: '%s' is not an in-scope local array", nametok);
        if (arrd->array_len == 0) failf("claloadN: '%s' is a scalar local, not an array (use load/store instead)", nametok);
        if (esz != arrd->size_bytes) failf("claloadN: width doesn't match how '%s' was declared", nametok);

        operand_t arrop;
        parse__operand(nametok, &arrop);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_LALOAD;
        i.elem_size = esz;
        i.src = arrop;
        if (is__number(idxtok)) {
            long lit = parse__number(idxtok);
            if (lit < 0 || lit >= arrd->array_len)
                failf("claloadN: literal index out of bounds for '%s'", nametok);
            i.idx_reg.kind = OPND_IMM;
            i.idx_reg.imm = lit;
        } else {
            if (!parse__register(idxtok, &i.idx_reg))
                failf("claloadN: '%s' is not a valid register or integer index", idxtok);
            emit_array_bounds_check(i.idx_reg, arrd->array_len);
        }
        parse__operand(dsttok, &i.dst);
        if (i.dst.kind != OPND_REG) fail("claloadN: destination must be a register");
        push__instr(i);
        return 1;
    }

    /* clastoreN rSRC > name[rIDX]; -- bounds-checked sibling of
       lastoreN, mirroring claloadN above the same way lastoreN mirrors
       laloadN. See claloadN's comment for the full rationale. */
    if (strncmp(tokens[0], "clastore", 8) == 0 && isdigit((unsigned char)tokens[0][8])) {
        int bits = atoi(tokens[0] + 8);
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8)
            failf("'%s' has invalid width: use clastore8/16/32/64", tokens[0]);

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char srctok[MAX_SYMLEN], nametok[MAX_SYMLEN], idxtok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63s > %63[^[][ %63[^]] ] ", srctok, nametok, idxtok) != 3)
            fail("malformed 'clastoreN': expected 'clastoreN rSRC > name[rIDX];'");
        char *close_check3 = strchr(buf, ']');
        if (!close_check3) fail("malformed 'clastoreN': missing ']'");

        decl_t *arrd = find__decl(nametok);
        if (!arrd || arrd->section != SEC_LOCAL) failf("clastoreN: '%s' is not an in-scope local array", nametok);
        if (arrd->array_len == 0) failf("clastoreN: '%s' is a scalar local, not an array (use load/store instead)", nametok);
        if (esz != arrd->size_bytes) failf("clastoreN: width doesn't match how '%s' was declared", nametok);

        operand_t arrop;
        parse__operand(nametok, &arrop);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_LASTORE;
        i.elem_size = esz;
        i.dst = arrop;
        parse__operand(srctok, &i.src);
        if (is__number(idxtok)) {
            long lit = parse__number(idxtok);
            if (lit < 0 || lit >= arrd->array_len)
                failf("clastoreN: literal index out of bounds for '%s'", nametok);
            i.idx_reg.kind = OPND_IMM;
            i.idx_reg.imm = lit;
        } else {
            if (!parse__register(idxtok, &i.idx_reg))
                failf("clastoreN: '%s' is not a valid register or integer index", idxtok);
            emit_array_bounds_check(i.idx_reg, arrd->array_len);
        }
        push__instr(i);
        return 1;
    }


    /* Atomics: atom+/atom-/atom&/atom|/atom^/atom<>/atom></atom<>< SRC > SYM > rDST
       [%ORDERING];
       Fetch-and-op on a volatile/bss global or local. SYM's declared
       width (8/16/32/64) is used as-is; codegen infers it the same way
       load/store already do (see operand_mem_size), so there's no width
       suffix on the mnemonic the way laloadN/istoreN have one -- SYM
       already carries its own width, unlike an array where the same name
       could in principle be read at multiple widths. rDST receives the
       location's value from *before* the op was applied. Optional
       trailing '%ORDERING' (see parse_mem_order_suffix) requests a
       weaker-than-default memory ordering; omitted, every op here is
       sequentially consistent. */
    {
        static const struct { const char *name; opcode_t op; } atomic_rmw_ops[] = {
            {"atom+",  OP_ATOMIC_ADD},
            {"atom-",  OP_ATOMIC_SUB},
            {"atom&",  OP_ATOMIC_AND},
            {"atom|",  OP_ATOMIC_OR},
            {"atom^",  OP_ATOMIC_XOR},
            {"atom<>", OP_ATOMIC_SWAP},
            {"atom><", OP_ATOMIC_MAX},
            {"atom<><", OP_ATOMIC_MIN},
        };
        for (size_t k = 0; k < sizeof(atomic_rmw_ops)/sizeof(atomic_rmw_ops[0]); k++) {
            if (strcmp(tokens[0], atomic_rmw_ops[k].name) != 0) continue;

            char buf[MAX_LINE];
            strncpy(buf, raw_trimmed, sizeof(buf) - 1);
            buf[sizeof(buf)-1] = '\0';
            char srctok[MAX_SYMLEN], symtok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
            if (sscanf(buf, "%*s %63s > %63s > %63s", srctok, symtok, dsttok) != 3) {
                char msg[128];
                snprintf(msg, sizeof(msg), "malformed '%s': expected '%s SRC > SYM > rDST;'",
                         atomic_rmw_ops[k].name, atomic_rmw_ops[k].name);
                fail(msg);
            }
            strip__semicolon(dsttok);
            /* dsttok may have grabbed a trailing '%ORDERING' too (sscanf's
               %s stops at whitespace, and the suffix is written with a
               space before it: '... > rDST %relaxed;') -- strip it back
               off dsttok before operand-parsing rDST, since
               parse_mem_order_suffix reads the ordering from the whole
               line separately, not from this token. */
            char *pct_in_dst = strchr(dsttok, '%');
            if (pct_in_dst) *pct_in_dst = '\0';

            instr_t i; memset(&i, 0, sizeof(i));
            i.op = atomic_rmw_ops[k].op;
            i.mem_order = parse_mem_order_suffix(raw_trimmed, atomic_rmw_ops[k].name);
            parse__operand(srctok, &i.src);
            if (is_mem_operand(i.src.kind)) {
                char msg[128];
                snprintf(msg, sizeof(msg), "'%s': SRC must be a register or immediate, not a bare symbol (load it into a register first)", atomic_rmw_ops[k].name);
                fail(msg);
            }
            parse__operand(symtok, &i.dst);
            if (!is_mem_operand(i.dst.kind)) {
                char msg[160];
                snprintf(msg, sizeof(msg), "'%s': SYM must be a volatile/bss global or local, not '%s'", atomic_rmw_ops[k].name, symtok);
                fail(msg);
            }
            parse__operand(dsttok, &i.result_reg);
            if (i.result_reg.kind != OPND_REG) {
                char msg[96];
                snprintf(msg, sizeof(msg), "'%s': rDST must be a register", atomic_rmw_ops[k].name);
                fail(msg);
            }
            push__instr(i);
            return 1;
        }
    }

    /* atom= SYM, rEXPECTED, rDESIRED > rDST [%ORDERING];
       Compare-and-swap. If SYM currently equals rEXPECTED, atomically
       stores rDESIRED into SYM and sets rDST = 1; otherwise leaves SYM
       unchanged and sets rDST = 0. This is the "strong" CAS form (a
       spurious failure is never reported) -- Chard has no weak/spurious-
       failure variant, since that's purely a performance escape hatch on
       LL/SC targets (AArch64/RISC-V) and would have no meaning to give
       x86-64's lock cmpxchg, which never fails spuriously to begin
       with; exposing it would mean one target could observe behavior
       the other two can't produce. Optional trailing '%ORDERING' (see
       parse_mem_order_suffix) same as the RMW ops above. */
    if (strcmp(tokens[0], "atom=") == 0) {
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char symtok[MAX_SYMLEN], exptok[MAX_SYMLEN], destok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63[^,], %63[^,], %63s > %63s", symtok, exptok, destok, dsttok) != 4)
            fail("malformed 'atom=': expected 'atom= SYM, rEXPECTED, rDESIRED > rDST;'");
        strip__semicolon(dsttok);
        char *pct_in_dst = strchr(dsttok, '%'); /* see the RMW block's identical strip, above */
        if (pct_in_dst) *pct_in_dst = '\0';

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_ATOMIC_CAS;
        i.mem_order = parse_mem_order_suffix(raw_trimmed, "atom=");
        parse__operand(symtok, &i.dst);
        if (!is_mem_operand(i.dst.kind))
            failf("'atom=': SYM must be a volatile/bss global or local, not '%s'", symtok);
        if (!parse__register(exptok, &i.cas_expected))
            failf("'atom=': rEXPECTED must be a register, not '%s'", exptok);
        if (!parse__register(destok, &i.cas_desired))
            failf("'atom=': rDESIRED must be a register, not '%s'", destok);
        parse__operand(dsttok, &i.result_reg);
        if (i.result_reg.kind != OPND_REG)
            fail("'atom=': rDST must be a register");
        push__instr(i);
        return 1;
    }

    /* i2s rSRC > rBUF, rLEN;  convert integer rSRC to ASCII decimal
       digits written at address rBUF; rLEN receives the byte count
       written. See the OP_I2S comment in the opcode_t enum for the
       full contract (no null terminator, rBUF may be any writable
       pointer). rSRC/rBUF/rLEN must all be distinct integer registers:
       codegen's digit loop writes rBUF/rLEN as it goes, so an alias
       (e.g. 'i2s r1 > r1, r2;') would have the source integer
       clobbered mid-conversion by its own output pointer. */
    if (strcmp(tokens[0], "i2s") == 0) {
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char srctok[MAX_SYMLEN], buftok[MAX_SYMLEN], lentok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63[^>] > %63[^,], %63s", srctok, buftok, lentok) != 3)
            fail("malformed 'i2s': expected 'i2s rSRC > rBUF, rLEN;'");
        trim(srctok);
        strip__semicolon(lentok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_I2S;
        if (!parse__register(srctok, &i.src) || i.src.is_float)
            failf("'i2s': rSRC must be an integer register, not '%s'", srctok);
        if (!parse__register(buftok, &i.dst) || i.dst.is_float)
            failf("'i2s': rBUF must be an integer register, not '%s'", buftok);
        if (!parse__register(lentok, &i.len_reg) || i.len_reg.is_float)
            failf("'i2s': rLEN must be an integer register, not '%s'", lentok);
        if (i.src.reg_num == i.dst.reg_num && i.src.is_sp == i.dst.is_sp)
            fail("'i2s': rSRC and rBUF must be distinct registers");
        if (i.src.reg_num == i.len_reg.reg_num && i.src.is_sp == i.len_reg.is_sp)
            fail("'i2s': rSRC and rLEN must be distinct registers");
        if (i.dst.reg_num == i.len_reg.reg_num && i.dst.is_sp == i.len_reg.is_sp)
            fail("'i2s': rBUF and rLEN must be distinct registers");
        push__instr(i);
        return 1;
    }

    /* s2i rBUF, rLEN > rDST;  parse rLEN bytes of ASCII decimal digits
       (optional leading '-') starting at address rBUF, writing the
       result to rDST. See the OP_S2I comment in the opcode_t enum for
       the full contract. rBUF/rLEN/rDST must all be distinct integer
       registers, for the same reason i2s requires it: codegen walks
       rBUF and counts down rLEN while accumulating into rDST, so any
       alias among them would have the accumulator or loop counter
       overwritten mid-parse. */
    if (strcmp(tokens[0], "s2i") == 0) {
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char buftok[MAX_SYMLEN], lentok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63[^,], %63[^>] > %63s", buftok, lentok, dsttok) != 3)
            fail("malformed 's2i': expected 's2i rBUF, rLEN > rDST;'");
        trim(lentok);
        strip__semicolon(dsttok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_S2I;
        if (!parse__register(buftok, &i.dst) || i.dst.is_float)
            failf("'s2i': rBUF must be an integer register, not '%s'", buftok);
        if (!parse__register(lentok, &i.len_reg) || i.len_reg.is_float)
            failf("'s2i': rLEN must be an integer register, not '%s'", lentok);
        if (!parse__register(dsttok, &i.result_reg) || i.result_reg.is_float)
            failf("'s2i': rDST must be an integer register, not '%s'", dsttok);
        if (i.dst.reg_num == i.len_reg.reg_num && i.dst.is_sp == i.len_reg.is_sp)
            fail("'s2i': rBUF and rLEN must be distinct registers");
        if (i.dst.reg_num == i.result_reg.reg_num && i.dst.is_sp == i.result_reg.is_sp)
            fail("'s2i': rBUF and rDST must be distinct registers");
        if (i.len_reg.reg_num == i.result_reg.reg_num && i.len_reg.is_sp == i.result_reg.is_sp)
            fail("'s2i': rLEN and rDST must be distinct registers");
        push__instr(i);
        return 1;
    }

    /* bcmpN rDST, rPTR1, rPTR2, LEN;  byte-equality compare over LEN
       bytes starting at rPTR1/rPTR2 -- rDST = 0 if equal, 1 if not
       (bcmp/memcmp convention; see the OP_BCMP comment in the opcode_t
       enum for the full contract, including why equal=0 rather than a
       boolean "are they equal" flag). N (8/16/32/64, same digit-suffix
       convention as laloadN/lastoreN/iloadN/istoreN) picks codegen's
       per-iteration comparison width; it's purely how the loop reads
       memory, not a second meaning for LEN, which always counts bytes.
       LEN may be a register or an integer immediate (mirrors
       laloadN/lastoreN's index operand, parsed the same way via
       parse__operand + is__number, rather than i2s/s2i's
       register-only rLEN, since here there's no operand-count pressure
       forcing register-only the way i2s/s2i's 3-register contract
       does). rDST/rPTR1/rPTR2 must be three distinct integer
       registers -- codegen's comparison loop walks rPTR1/rPTR2 and
       writes rDST as it goes, so any alias would corrupt the walk,
       same reasoning as i2s/s2i's own distinct-register checks. */
    if (strncmp(tokens[0], "bcmp", 4) == 0 && isdigit((unsigned char)tokens[0][4])) {
        int bits = atoi(tokens[0] + 4);
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8)
            failf("'%s' has invalid width: use bcmp8/16/32/64", tokens[0]);

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char dsttok[MAX_SYMLEN], p1tok[MAX_SYMLEN], p2tok[MAX_SYMLEN], lentok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63[^,], %63[^,], %63[^,], %63s", dsttok, p1tok, p2tok, lentok) != 4)
            fail("malformed 'bcmpN': expected 'bcmpN rDST, rPTR1, rPTR2, LEN;'");
        trim(p1tok); trim(p2tok);
        strip__semicolon(lentok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_BCMP;
        i.elem_size = esz;
        if (!parse__register(dsttok, &i.dst) || i.dst.is_float)
            failf("'bcmpN': rDST must be an integer register, not '%s'", dsttok);
        if (!parse__register(p1tok, &i.src) || i.src.is_float)
            failf("'bcmpN': rPTR1 must be an integer register, not '%s'", p1tok);
        if (!parse__register(p2tok, &i.base_reg) || i.base_reg.is_float)
            failf("'bcmpN': rPTR2 must be an integer register, not '%s'", p2tok);
        if (i.dst.reg_num == i.src.reg_num && i.dst.is_sp == i.src.is_sp)
            fail("'bcmpN': rDST and rPTR1 must be distinct registers");
        if (i.dst.reg_num == i.base_reg.reg_num && i.dst.is_sp == i.base_reg.is_sp)
            fail("'bcmpN': rDST and rPTR2 must be distinct registers");
        if (i.src.reg_num == i.base_reg.reg_num && i.src.is_sp == i.base_reg.is_sp)
            fail("'bcmpN': rPTR1 and rPTR2 must be distinct registers");

        if (is__number(lentok)) {
            long lit = parse__number(lentok);
            if (lit < 0) fail("'bcmpN': LEN must be non-negative");
            i.len_reg.kind = OPND_IMM;
            i.len_reg.imm = lit;
        } else if (parse__register(lentok, &i.len_reg)) {
            if (i.len_reg.is_float) fail("'bcmpN': LEN register must be an integer register");
            if (i.len_reg.reg_num == i.dst.reg_num && i.len_reg.is_sp == i.dst.is_sp)
                fail("'bcmpN': LEN and rDST must be distinct registers");
            if (i.len_reg.reg_num == i.src.reg_num && i.len_reg.is_sp == i.src.is_sp)
                fail("'bcmpN': LEN and rPTR1 must be distinct registers");
            if (i.len_reg.reg_num == i.base_reg.reg_num && i.len_reg.is_sp == i.base_reg.is_sp)
                fail("'bcmpN': LEN and rPTR2 must be distinct registers");
        } else {
            failf("'bcmpN': LEN must be a register or an integer literal, not '%s'", lentok);
        }

        push__instr(i);
        return 1;
    }

    /* bcopyN rDST, rSRC, LEN;  -- see OP_BCOPY. Mirrors bcmpN's parse
       shape immediately above (same sscanf-based comma splitting,
       same LEN-is-register-or-immediate handling), just with one
       fewer operand: no result register, since a copy has nothing to
       report back the way a comparison does. */
    if (strncmp(tokens[0], "bcopy", 5) == 0 && isdigit((unsigned char)tokens[0][5])) {
        int bits = atoi(tokens[0] + 5);
        int esz = bits / 8;
        if (esz != 1 && esz != 2 && esz != 4 && esz != 8)
            failf("'%s' has invalid width: use bcopy8/16/32/64", tokens[0]);

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char dsttok[MAX_SYMLEN], srctok[MAX_SYMLEN], lentok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63[^,], %63[^,], %63s", dsttok, srctok, lentok) != 3)
            fail("malformed 'bcopyN': expected 'bcopyN rDST, rSRC, LEN;'");
        trim(srctok);
        strip__semicolon(lentok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_BCOPY;
        i.elem_size = esz;
        if (!parse__register(dsttok, &i.dst) || i.dst.is_float)
            failf("'bcopyN': rDST must be an integer register, not '%s'", dsttok);
        if (!parse__register(srctok, &i.src) || i.src.is_float)
            failf("'bcopyN': rSRC must be an integer register, not '%s'", srctok);
        if (i.dst.reg_num == i.src.reg_num && i.dst.is_sp == i.src.is_sp)
            fail("'bcopyN': rDST and rSRC must be distinct registers");

        if (is__number(lentok)) {
            long lit = parse__number(lentok);
            if (lit < 0) fail("'bcopyN': LEN must be non-negative");
            i.len_reg.kind = OPND_IMM;
            i.len_reg.imm = lit;
        } else if (parse__register(lentok, &i.len_reg)) {
            if (i.len_reg.is_float) fail("'bcopyN': LEN register must be an integer register");
            if (i.len_reg.reg_num == i.dst.reg_num && i.len_reg.is_sp == i.dst.is_sp)
                fail("'bcopyN': LEN and rDST must be distinct registers");
            if (i.len_reg.reg_num == i.src.reg_num && i.len_reg.is_sp == i.src.is_sp)
                fail("'bcopyN': LEN and rSRC must be distinct registers");
        } else {
            failf("'bcopyN': LEN must be a register or an integer literal, not '%s'", lentok);
        }

        push__instr(i);
        return 1;
    }

    /* fence [%ORDERING]; -- memory barrier, no other operands. Default
       (no suffix) is a full sequentially-consistent barrier; an
       optional trailing '%ORDERING' (see parse_mem_order_suffix)
       requests a weaker one -- this is the "future scope" the OP_FENCE
       comment in the opcode_t enum originally flagged. */
    if (strcmp(tokens[0], "fence") == 0 || strcmp(tokens[0], "fence;") == 0) {
        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_FENCE;
        i.mem_order = parse_mem_order_suffix(raw_trimmed, "fence");
        /* Anything present that isn't the recognized '%ORDERING' suffix
           is a real extra operand -- parse_mem_order_suffix already
           fails loudly on a '%' followed by an unrecognized name, so
           this only needs to catch a *second* whitespace-separated
           token that has no '%' in it at all (e.g. 'fence foo;'). */
        if (ntok > 1 && strcmp(tokens[0], "fence;") != 0 && !strchr(raw_trimmed, '%'))
            fail("'fence' takes no operands (or an optional '%ORDERING' suffix)");
        push__instr(i);
        return 1;
    }

    /* raw "..."; -- escape hatch: emits the quoted text verbatim into
       the output at this point in the instruction stream, for
       whichever target is currently being emitted. Opaque to Chard --
       no parsing, validation, or register/stack bookkeeping is done
       on the text, so it's entirely the caller's responsibility to
       write something valid for the target(s) they intend to emit
       for (a raw AVX instruction will happily get copied into AArch64
       output verbatim if you compile this same source for both).
       Same quoted-string shape and escape resolution as 'ascii'
       declarations (see parse__decl), reusing the same \n/\t/\\/\"
       escape set so raw multi-instruction sequences can be split
       across lines with '\n' rather than requiring one 'raw' per
       line. */
    if (strcmp(tokens[0], "raw") == 0) {
        const char *q1 = strchr(raw_trimmed, '"');
        if (!q1) fail("'raw' requires a quoted string literal (raw \"...\";)");
        const char *q2 = find_string_close_quote(q1);
        if (!q2) fail("unterminated string literal");

        char resolved[MAX_STRLEN];
        int ri = 0;
        for (const char *p = q1 + 1; p < q2 && ri < MAX_STRLEN - 1; p++) {
            if (*p == '\\' && p + 1 < q2) {
                p++;
                char c = *p == 'n' ? '\n' : *p == 't' ? '\t' :
                         *p == '\\' ? '\\' : *p == '"' ? '"' : *p;
                resolved[ri++] = c;
            } else {
                resolved[ri++] = *p;
            }
        }
        resolved[ri] = '\0';

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_RAW;
        memcpy(i.raw_text, resolved, ri + 1);
        push__instr(i);
        return 1;
    }

    /* bytes iK v1, v2, ...; -- emits a raw, anonymous sequence of iK-sized
       values directly into the instruction stream at this point: a
       db/dw/dd/dq-equivalent dropped inline rather than declared up front
       the way 'data iK name[] = ...;' is. This is what lets a jump table
       or lookup table sit right next to the code that indexes into it,
       the way hand-written assembly commonly does, instead of forcing it
       into section .data far from its point of use. It has no name of
       its own -- to address it (e.g. to jump through a table), precede
       the statement with a bare '@label:' (no trailing '{'), which is
       already an ordinary mid-block jump-target label with no frame of
       its own; see the '@label' parsing above.
       Same comma-separated-list shape as 'data iK name[] = v1, v2, ...;'
       (see parse__decl's own comment for why a plain comma list rather
       than a brace-delimited one: split__statements already treats '{' as
       a statement boundary, so a literal brace here would have been
       sliced apart before this code ever saw it).
       'bytes fK v1.v, v2.v, ...;' is the float-element sibling, mirroring
       'data fK name[] = ...;' alongside 'data iK name[] = ...;': the size
       specifier's leading letter (checked below) picks integer vs. float,
       and float values are then required to be float literals and stored
       in raw_data_fvals instead of raw_data_vals. */
    if (strcmp(tokens[0], "bytes") == 0) {
        if (ntok < 2) fail("expected size specifier after 'bytes' (e.g. bytes i32 1, 2, 3; or bytes f64 1.0, 2.5;)");
        const char *szspec = resolve_size_alias(tokens[1]);
        int is_float = 0;
        if (szspec[0] == 'f' && isdigit((unsigned char)szspec[1])) {
            is_float = 1;
            g_uses_float = 1;
        } else if (szspec[0] != 'i' || !isdigit((unsigned char)szspec[1])) {
            fail("'bytes' requires a size specifier (e.g. i8/char, i32/int, f32, f64)");
        }
        int dsize = atoi(szspec + 1) / 8;
        if (is_float) {
            if (dsize != 4 && dsize != 8)
                fail("'bytes' float size specifier must be f32 or f64");
        } else {
            if (dsize != 1 && dsize != 2 && dsize != 4 && dsize != 8)
                fail("'bytes' integer size specifier must be i8/char, i16/short, i32/int, or i64/long");
        }

        /* Positional lookup below deliberately searches for tokens[1]
           as WRITTEN in the source (e.g. "long"), not szspec's resolved
           "i64" -- szspec only exists to feed the size check above;
           the alias is never actually present in raw_trimmed, so
           searching for it there would never match. */
        const char *sizetok = strstr(raw_trimmed, tokens[1]);
        if (!sizetok) fail("malformed 'bytes' statement");
        const char *listtext = sizetok + strlen(tokens[1]);

        char listbuf[MAX_LINE];
        strncpy(listbuf, listtext, MAX_LINE - 1);
        listbuf[MAX_LINE - 1] = '\0';
        char *semi = strchr(listbuf, ';');
        if (semi) *semi = '\0';

        long *vals = NULL; double *fvals = NULL;
        int vals_cap = 0, fvals_cap = 0;
        /* '&label' jump-table entries -- same mechanism as 'data iK
           name[] = ...;' (see that parsing branch's comment for the
           full rationale, including why is_label/val_labels each need
           their own DA_ENSURE capacity variable rather than sharing
           vals_cap). This is what lets a jump table be dropped inline
           right next to the code that indexes into it, addressed via a
           preceding bare '@label:' -- see this branch's own top
           comment. */
        int *is_label = NULL; char **val_labels = NULL;
        int is_label_cap = 0, val_labels_cap = 0;
        int any_label = 0;
        int nvals = 0;
        char *tok = strtok(listbuf, ",");
        while (tok) {
            char *vt = trim(tok);
            if (*vt != '\0') {
                if (*vt == '&') {
                    char *lbl = trim(vt + 1);
                    if (*lbl == '\0') fail("'bytes': '&' must be followed by a label name (got bare '&')");
                    if (is_float) failf("'bytes': '&%s' (a label address) cannot be an 'fK' element -- jump-table entries must be an integer size (i64 for a full address)", lbl);
                    DA_ENSURE(vals, vals_cap, nvals, long);
                    DA_ENSURE(is_label, is_label_cap, nvals, int);
                    DA_ENSURE(val_labels, val_labels_cap, nvals, char *);
                    vals[nvals] = 0;
                    is_label[nvals] = 1;
                    val_labels[nvals] = str_dup(lbl);
                    any_label = 1;
                    nvals++;
                } else if (is_float) {
                    if (!is_float_literal(vt))
                        failf("'bytes': values must be plain float literals (got '%s')", vt);
                    DA_ENSURE(fvals, fvals_cap, nvals, double);
                    fvals[nvals++] = atof(vt);
                } else {
                    if (!is__number(vt))
                        failf("'bytes': values must be plain integer literals or '&label' jump-table entries (got '%s')", vt);
                    DA_ENSURE(vals, vals_cap, nvals, long);
                    DA_ENSURE(is_label, is_label_cap, nvals, int);
                    DA_ENSURE(val_labels, val_labels_cap, nvals, char *);
                    vals[nvals] = parse__number(vt);
                    is_label[nvals] = 0;
                    val_labels[nvals] = NULL;
                    nvals++;
                }
            }
            tok = strtok(NULL, ",");
        }
        if (nvals == 0) fail("'bytes' requires at least one value (e.g. bytes i32 1, 2, 3;)");

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_RAWDATA;
        i.raw_data_size = dsize;
        i.raw_data_is_float = is_float;
        i.raw_data_nvals = nvals;
        i.raw_data_vals = vals;
        i.raw_data_vals_cap = vals_cap;
        i.raw_data_fvals = fvals;
        i.raw_data_fvals_cap = fvals_cap;
        if (!is_float && any_label) {
            i.raw_data_val_is_label = is_label;
            i.raw_data_val_labels = val_labels;
        } else {
            free(is_label);
            free(val_labels);
        }
        push__instr(i);
        return 1;
    }

    /* push SRC; -- pushes a register or immediate onto the stack and
       decrements sp by 8. SRC may not be a bare symbol name: Chard has
       no "push the value at this memory location" form, matching how
       every other arithmetic op requires an explicit load first. */
    if (strcmp(tokens[0], "push") == 0 || strcmp(tokens[0], "push;") == 0) {
        if (ntok < 2) fail("'push' requires an operand (push SRC;)");
        if (ntok > 2) fail("'push' takes exactly one operand (push SRC;)");
        char srctok[MAX_SYMLEN];
        strncpy(srctok, tokens[1], MAX_SYMLEN - 1);
        srctok[MAX_SYMLEN - 1] = '\0';
        strip__semicolon(srctok);
        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_PUSH;
        parse__operand(srctok, &i.src);
        if (is_mem_operand(i.src.kind))
            fail("'push' requires a register or immediate operand, not a bare symbol (load it into a register first)");
        push__instr(i);
        return 1;
    }

    /* pop; or pop > rX; -- pops the top of the stack and increments sp
       by 8. The destination is optional: 'pop;' alone just discards the
       popped value (still moving sp), useful for balancing a stack
       after a push whose value is no longer needed. */
    if (strcmp(tokens[0], "pop") == 0 || strcmp(tokens[0], "pop;") == 0) {
        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_POP;
        if (ntok >= 3 && strcmp(tokens[1], ">") == 0) {
            if (ntok > 3) fail("'pop' expects either 'pop;' or 'pop > rX;'");
            char dsttok[MAX_SYMLEN];
            strncpy(dsttok, tokens[2], MAX_SYMLEN - 1);
            dsttok[MAX_SYMLEN - 1] = '\0';
            strip__semicolon(dsttok);
            parse__operand(dsttok, &i.dst);
            if (i.dst.kind != OPND_REG) fail("'pop' destination must be a register (pop > rX;)");
        } else if (ntok >= 2) {
            fail("'pop' expects either 'pop;' or 'pop > rX;'");
        }
        push__instr(i);
        return 1;
    }

    /* spill rX; -- sets rX aside on the current block's frame (as a
       hidden, anonymous local -- see the "Function parameters" section
       for the sibling idea of aliasing a name to a register; this is
       the reverse: giving a register's current value a home in memory
       so the register itself is free to reuse) so the caller can put
       something else in rX for a while. Explicit and manual by design:
       Chard has no automatic register allocator, so running out of the
       12 general registers is ordinarily a hard parse error (see
       parse__register) -- spill/unspill is the escape hatch, written by
       hand at exactly the point the programmer knows they need the
       room, the same explicitness Chard already expects everywhere
       else.

       spill mylocal; -- the same idea for a local's own storage instead
       of a register: copies mylocal's current value into a hidden slot
       (the original local is untouched, just "checked out" -- free for
       the programmer to overwrite in the meantime), so unspill can put
       the original value back later. This is the memory-side analog of
       spilling a register: same problem (need this storage for
       something else for a while, then want the original value back
       unharmed), same stack discipline. Scalars only, not array
       elements -- see declare_local_array for why an element reference
       needs an index operand this doesn't carry.

       Both forms share one block-level stack -- last spilled, first
       unspilled -- enforced below, regardless of whether each entry
       is a register or a local. */
    if (strcmp(tokens[0], "spill") == 0 || strcmp(tokens[0], "spill;") == 0) {
        if (ntok != 2) fail("'spill' requires exactly one operand: a register (spill rX;) or a local (spill myname;)");
        char srctok[MAX_SYMLEN];
        strncpy(srctok, tokens[1], MAX_SYMLEN - 1);
        srctok[MAX_SYMLEN - 1] = '\0';
        strip__semicolon(srctok);
        operand_t src;
        parse__operand(srctok, &src);
        int is_local = (src.kind == OPND_LOCAL);
        if (!is_local && (src.kind != OPND_REG || src.is_sp))
            fail("'spill' requires a register (spill rX;) or a local (spill myname;), not sp or anything else");
        if (is_local && src.local_size != 8) {
            char msg[MAX_SYMLEN + 96];
            snprintf(msg, sizeof(msg), "'spill' only supports 8-byte locals for now (spill myname;); '%s' is %d bytes", src.sym, src.local_size);
            fail(msg);
        }
        if (!in_local_frame()) fail("'spill' may only be used inside a @label { ... } block");

        local_frame_t *f = current_local_frame();
        DA_ENSURE(f->spill_is_local, f->spill_cap, f->nspilled, int);
        /* the three arrays below share f->nspilled/f->spill_cap with
           spill_is_local above and are always grown in lockstep, so a
           second DA_ENSURE here would be redundant -- just match sizes */
        {
            int need_cap = f->spill_cap;
            f->spill_regs = realloc(f->spill_regs, (size_t)need_cap * sizeof(int));
            f->spill_local_decl_idx = realloc(f->spill_local_decl_idx, (size_t)need_cap * sizeof(int));
            f->spill_decl_idx = realloc(f->spill_decl_idx, (size_t)need_cap * sizeof(int));
            if (!f->spill_regs || !f->spill_local_decl_idx || !f->spill_decl_idx) {
                perror("realloc"); exit(1);
            }
        }

        char slotname[MAX_SYMLEN];
        snprintf(slotname, sizeof(slotname), "__spill%d", g_spill_counter++);
        decl_t *d = declare__local(slotname, 8); /* 8 bytes: spills always
                                                    save a full register (or
                                                    a full 8-byte local's
                                                    value), regardless of
                                                    what the caller happens
                                                    to be using it for */

        operand_t store_src = src; /* a register directly, or -- for a
                                       local -- filled in just below */
        if (is_local) {
            /* OP_STORE.src is always assumed to be a register by codegen
               (see width_reg_name/reg__name), the same way plain 'mv'
               can't touch memory directly -- a local's value has to be
               loaded into a register first. Borrows g_init_scratch_reg
               (r12 by default, programmer-overridable via
               '%iscratchr rN;'), the same bounce point 'local i8
               x = 5;' already uses to seed an initializer into a local
               (see its handling above): an ordinary user-addressable
               register, not one reserved from the programmer, so this
               spill/unspill bounce is only safe against the same
               register the programmer chose (or left at the default)
               for %iscratchr. */
            instr_t seed; memset(&seed, 0, sizeof(seed));
            seed.op = OP_LOAD;
            seed.src = src; /* the OPND_LOCAL operand parse__operand resolved above */
            seed.dst.kind = OPND_REG;
            seed.dst.reg_num = g_init_scratch_reg;
            push__instr(seed);
            store_src = seed.dst;
        }

        instr_t store; memset(&store, 0, sizeof(store));
        store.op = OP_STORE;
        store.src = store_src;
        store.dst.kind = OPND_LOCAL;
        strncpy(store.dst.sym, slotname, MAX_SYMLEN - 1); /* diagnostics only */
        store.dst.local_offset = d->local_offset;
        store.dst.local_size = d->size_bytes;
        store.dst.frames_up = 0; /* always the innermost frame: spill/unspill
                                     is a same-block stack discipline */
        push__instr(store);

        f->spill_decl_idx[f->nspilled] = (int)(d - decls);
        f->spill_is_local[f->nspilled] = is_local;
        if (is_local) {
            decl_t *srcdecl = find__decl(src.sym);
            f->spill_local_decl_idx[f->nspilled] = (int)(srcdecl - decls);
            f->spill_regs[f->nspilled] = 0; /* unused for a local entry */
        } else {
            f->spill_regs[f->nspilled] = src.reg_num;
            f->spill_local_decl_idx[f->nspilled] = -1; /* unused for a register entry */
        }
        f->nspilled++;
        return 1;
    }

    /* unspill rX; or unspill rX > rY; -- reclaims the most recently
       spilled register's saved value, restoring it into rX by default
       or into rY if given (e.g. the caller moved on to using a
       different register for the same purpose in the meantime).

       unspill myname; -- the local counterpart: restores the most
       recently spilled local's saved value back into that same local.
       No '> other' redirect form for this case -- restoring a local's
       own saved value into some *different* local doesn't correspond
       to anything a caller would actually want (unlike a register,
       which is genuinely fungible, a local is a specific named slot
       the rest of the block already refers to by that name), so it's
       simply not offered.

       Either way, the named operand must match whatever is on top of
       this block's spill stack -- last spilled, first unspilled --
       since that's the only saved value this instruction has any
       record of; unspilling out of order, or naming a local when a
       register is on top (or vice versa), is rejected at parse time
       with a message naming what actually needs to come off first,
       rather than silently restoring the wrong value. */
    if (strcmp(tokens[0], "unspill") == 0) {
        if (ntok < 2) fail("'unspill' requires an operand: a register (unspill rX; or unspill rX > rY;) or a local (unspill myname;)");
        char srctok[MAX_SYMLEN];
        strncpy(srctok, tokens[1], MAX_SYMLEN - 1);
        srctok[MAX_SYMLEN - 1] = '\0';
        strip__semicolon(srctok);
        operand_t target;
        parse__operand(srctok, &target);
        int is_local = (target.kind == OPND_LOCAL);
        if (!is_local && (target.kind != OPND_REG || target.is_sp))
            fail("'unspill' requires a register (unspill rX;) or a local (unspill myname;), not sp or anything else");
        if (!in_local_frame()) fail("'unspill' may only be used inside a @label { ... } block");

        local_frame_t *f = current_local_frame();
        if (f->nspilled == 0) fail("'unspill' with nothing spilled in this block");
        int top_idx = f->nspilled - 1;
        int top_is_local = f->spill_is_local[top_idx];

        if (is_local != top_is_local) {
            if (top_is_local) {
                decl_t *topd = &decls[f->spill_local_decl_idx[top_idx]];
                failf("'unspill' must match the most recently spilled local ('%s'), not a register", topd->name);
            } else {
                char regbuf[16];
                snprintf(regbuf, sizeof(regbuf), "r%d", f->spill_regs[top_idx]);
                failf("'unspill' must match the most recently spilled register (%s)", regbuf);
            }
        }
        if (is_local) {
            decl_t *topd = &decls[f->spill_local_decl_idx[top_idx]];
            if (strcmp(target.sym, topd->name) != 0)
                failf("'unspill' must match the most recently spilled local ('%s')", topd->name);
        } else {
            int top_reg = f->spill_regs[top_idx];
            if (target.reg_num != top_reg) {
                char regbuf[16];
                snprintf(regbuf, sizeof(regbuf), "r%d", top_reg);
                failf("'unspill' must match the most recently spilled register (%s)", regbuf);
            }
        }

        operand_t dest = target;
        if (ntok >= 3) {
            if (is_local) fail("'unspill' has no '> other' form for a local (unspill myname;) -- it always restores back into the same local");
            if (ntok != 4 || strcmp(tokens[2], ">") != 0) fail("'unspill' expects 'unspill rX;' or 'unspill rX > rY;'");
            char dsttok[MAX_SYMLEN];
            strncpy(dsttok, tokens[3], MAX_SYMLEN - 1);
            dsttok[MAX_SYMLEN - 1] = '\0';
            strip__semicolon(dsttok);
            parse__operand(dsttok, &dest);
            if (dest.kind != OPND_REG || dest.is_sp) fail("'unspill' destination must be a register (unspill rX > rY;), not sp or anything else");
        }

        /* The slot's decl is still findable in decls[] (declare__local
           left it there; nothing removes an individual local before its
           whole block closes -- see close_local_frame), so its
           offset/size can be read back directly via the exact index
           spill() recorded for this stack position, rather than
           searching decls[] for "the most recent __spillN": that search
           would find whichever __spill local happens to be last in the
           array, which is not necessarily the one this unspill actually
           needs once more than one spill/unspill cycle has happened in
           this block (an already-unspilled slot's decl is still sitting
           there, undisturbed, since only a whole block's close reclaims
           decls[] in bulk). */
        decl_t *d = &decls[f->spill_decl_idx[top_idx]];

        instr_t load; memset(&load, 0, sizeof(load));
        load.op = OP_LOAD;
        load.src.kind = OPND_LOCAL;
        strncpy(load.src.sym, d->name, MAX_SYMLEN - 1);
        load.src.local_offset = d->local_offset;
        load.src.local_size = d->size_bytes;
        load.src.frames_up = 0;
        if (is_local) {
            /* Mirror spill's g_init_scratch_reg bounce (see above):
               OP_LOAD.dst is assumed to be a register by codegen, so
               restoring straight into a local's memory slot isn't a
               single-instruction move here either. Load the saved
               value into g_init_scratch_reg, then store it into the
               local. */
            load.dst.kind = OPND_REG;
            load.dst.reg_num = g_init_scratch_reg;
            push__instr(load);

            instr_t store; memset(&store, 0, sizeof(store));
            store.op = OP_STORE;
            store.src = load.dst;
            store.dst = target; /* the OPND_LOCAL operand parse__operand
                                    resolved above; a local always
                                    restores back into itself */
            push__instr(store);
        } else {
            load.dst = dest;
            push__instr(load);
        }

        f->nspilled--;
        return 1;
    }

    /* inc NAME; / dec NAME; -- shorthand for the load/add-or-sub-1/store
       triple a plain 'i = i + 1;'-style increment otherwise needs
       spelled out by hand (there's no operator-on-memory shortcut
       elsewhere in Chard: OP_STORE.src and the arithmetic family's dst
       are both assumed to be registers by codegen, the same
       "everything detours through a register" rule spill/unspill's
       comment above already explains). Locals only, matching spill's
       own "scalars only, not array elements" scope -- a plain name
       operand is unambiguous (there's exactly one storage location,
       no index to resolve), while 'inc arr[i];' would need to decide
       whether i itself might change between the load and the store,
       which is exactly the kind of implicit-aliasing question Chard's
       explicit-by-design style (see spill/unspill's own comment)
       avoids by simply not offering the shorthand for that case --
       write it out by hand there instead (load arr[i] > rX; add 1 >
       rX; store rX > arr[i];). Borrows g_init_scratch_reg, the same
       bounce register spill/unspill and local-initializer seeding
       already use for this exact "local needs a register pit-stop"
       situation (see spill's comment above for why that's safe: an
       ordinary user-addressable register, not one reserved from the
       programmer). Desugars to exactly the three instructions a
       hand-written increment would use -- no new IR, no new opcode,
       just less to type for an extremely common operation. */
    if (strcmp(tokens[0], "inc") == 0 || strcmp(tokens[0], "dec") == 0) {
        int is_dec = (strcmp(tokens[0], "dec") == 0);
        if (ntok != 2)
            fail_fmt("'%s' requires exactly one operand: a local (%s myname;)", tokens[0], tokens[0]);
        char nametok[MAX_SYMLEN];
        strncpy(nametok, tokens[1], MAX_SYMLEN - 1);
        nametok[MAX_SYMLEN - 1] = '\0';
        strip__semicolon(nametok);
        operand_t target;
        parse__operand(nametok, &target);
        if (target.kind != OPND_LOCAL)
            fail_fmt("'%s' requires a local (%s myname;) -- not a register, immediate, or array element (write the load/%s/store out by hand for those)",
                     tokens[0], tokens[0], is_dec ? "sub" : "add");

        instr_t load; memset(&load, 0, sizeof(load));
        load.op = OP_LOAD;
        load.src = target;
        load.dst.kind = OPND_REG;
        load.dst.reg_num = g_init_scratch_reg;
        push__instr(load);

        instr_t op; memset(&op, 0, sizeof(op));
        op.op = is_dec ? OP_SUB : OP_ADD;
        op.src.kind = OPND_IMM;
        op.src.imm = 1;
        op.dst = load.dst;
        push__instr(op);

        instr_t store; memset(&store, 0, sizeof(store));
        store.op = OP_STORE;
        store.src = load.dst;
        store.dst = target;
        push__instr(store);

        return 1;
    }

    /* read(fd, buf, len) / write(fd, buf, len) -- named wrappers around
       the read/write syscalls, for when the caller wants full control
       over fd/buffer/length (out() covers the common "write an
       ascii-declared string to fd 1" case; these cover everything
       else: reading input, writing to arbitrary fds, writing raw
       buffers built by other means). Each argument may be a register,
       immediate, symbol, or local -- same generality as syscall(),
       since these desugar into the exact same 3-register-argument
       shape at codegen time, just with the syscall number already
       chosen instead of left as an explicit first argument. */
    if (strncmp(tokens[0], "read(", 5) == 0 || strncmp(tokens[0], "write(", 6) == 0) {
        int is_read = strncmp(tokens[0], "read(", 5) == 0;
        const char *name = is_read ? "read" : "write";
        if (g_mode == MODE_BARE) failf("'%s()' is a kernel syscall wrapper and is not available in '| mode bare;' (the default) -- add '| mode elf;' at the top of the file to use it", name);

        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char *open = strchr(buf, '(');
        char *close = strrchr(buf, ')');
        if (!open || !close || close < open) failf("malformed %s(): missing '(' or ')'", name);
        *close = '\0';
        char *argstr = open + 1;

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = is_read ? OP_READ : OP_WRITE;

        char *tok = strtok(argstr, ",");
        int n = 0;
        while (tok && n < 3) {
            char *a = trim(tok);
            if (*a != '\0') {
                parse__operand(a, &i.args[n]);
                n++;
            }
            tok = strtok(NULL, ",");
        }
        if (n != 3) failf("requires exactly 3 arguments: %s(fd, buf, len)", name);
        i.nargs = n;
        push__instr(i);
        return 1;
    }

    /* syscall(NUM, arg1, ..., arg6) -- raw escape hatch. Every argument
       actually passed is visible here; missing trailing args default to
       0, they are never silently invented. */
    if (strncmp(tokens[0], "syscall(", 8) == 0) {
        if (g_mode == MODE_BARE) fail("'syscall()' talks directly to the kernel and is not available in '| mode bare;' (the default) -- add '| mode elf;' at the top of the file to use it");
        /* re-join tokens from the raw line since args can be split by
           the ", " separator, then split on commas ourselves */
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char *open = strchr(buf, '(');
        char *close = strrchr(buf, ')');
        if (!open || !close || close < open) fail("malformed syscall(): missing '(' or ')'");
        *close = '\0';
        char *argstr = open + 1;

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_SYSCALL;

        char *tok = strtok(argstr, ",");
        int n = 0;
        while (tok && n < 7) {
            char *a = trim(tok);
            if (*a != '\0') {
                /* The syscall number (arg 0 only) may be spelled as a
                   name -- 'socket', 'bind', etc -- instead of a raw
                   number, either bare ('syscall(write, ...)') or
                   quoted ('syscall("write", ...)') -- accepting both
                   because a quoted name is the more natural spelling
                   for "this is a name, not code" (most languages quote
                   string-like arguments), and a bare identifier reads
                   more like it should resolve as a symbol the way
                   every other operand position treats one. A wrapping
                   '"..."' pair is stripped for arg 0 ONLY, purely to
                   decide whether the syscall-name lookup below applies
                   -- it does not make quoted strings a general operand
                   syntax elsewhere in syscall() or anywhere else in
                   Chard (see parse__operand, which has no notion of a
                   string literal operand at all: passing e.g.
                   syscall("write", 1, "hello", 5) would still send the
                   quotes straight through as part of an unquoted
                   symbol name for arg 2, same as always -- args past 0
                   are untouched by this stripping).
                   Checked ahead of parse__operand's default
                   bare-identifier handling (which would otherwise treat
                   an unrecognized name as OPND_SYM, i.e. a linker symbol
                   reference -- almost certainly not what was meant for a
                   syscall number). Only arg 0 gets this treatment:
                   syscall's remaining arguments are ordinary
                   register/immediate/symbol/local operands exactly as
                   before, so a symbol named e.g. 'bind' used as an actual
                   argument value elsewhere is unaffected. A register
                   (r1..r12/sp) or a plain number for arg 0 is untouched --
                   syscall_num_for_name only matches bare names, so this
                   is purely additive. */
                char namebuf[MAX_SYMLEN];
                const char *name_candidate = a;
                if (n == 0) {
                    size_t alen = strlen(a);
                    if (alen >= 2 && a[0] == '"' && a[alen - 1] == '"' && alen - 2 < sizeof(namebuf)) {
                        memcpy(namebuf, a + 1, alen - 2);
                        namebuf[alen - 2] = '\0';
                        name_candidate = namebuf;
                    }
                }
                long sysnum;
                int resolved = (n == 0 && !parse__register(a, &i.args[0]) && !is__number(a))
                                ? syscall_num_for_name(name_candidate, &sysnum) : 0;
                if (resolved == 1) {
                    i.args[0].kind = OPND_IMM;
                    i.args[0].imm = sysnum;
                } else if (resolved == -1) {
                    fail_fmt("syscall(\"%s\", ...): '%s' has no syscall number on this target "
                             "(see syscall_names' comment in chard.c for why) -- use a raw numeric "
                             "literal for this platform instead", name_candidate, name_candidate);
                } else {
                    parse__operand(a, &i.args[n]);
                }
                n++;
            }
            tok = strtok(NULL, ",");
        }
        if (n == 0) fail("syscall() requires at least a syscall number");
        i.nargs = n;
        push__instr(i);
        return 1;
    }

    /* call NAME(arg1, arg2, ...) [> rX];  -- a function-call
       expression, as opposed to the raw 'call SYM;' form below (which
       just emits the target's native call instruction with no argument
       marshalling at all). Arguments are moved into r1, r2, r3... in
       declaration order -- Chard's own convention, not any target's
       native ABI, so the exact same call site compiles identically on
       every backend (see the "Function parameters" section). Moves
       happen strictly left to right, one argument at a time; if an
       argument expression reads a register that an earlier argument's
       move already overwrote (e.g. 'call add(r2, r1)' when the
       function's own r1/r2 aren't yet what the caller means by them),
       the caller is responsible for staging values through locals or
       unused registers first -- Chard doesn't insert a hidden temporary
       to break such cycles, the same explicitness it already expects
       everywhere else (nothing here is different from how a raw
       multi-register sequence already behaves without 'call'). */
    if (strncmp(tokens[0], "call", 4) == 0 && (tokens[0][4] == '\0') && ntok >= 2 && strchr(tokens[1], '(')) {
        char buf[MAX_LINE];
        const char *paren_start = strstr(raw_trimmed, "call");
        paren_start = paren_start ? strchr(paren_start, '(') : NULL;
        if (!paren_start) fail("malformed call: missing '('");
        const char *name_start = raw_trimmed + 4;
        while (*name_start == ' ') name_start++;
        size_t namelen = (size_t)(paren_start - name_start);
        char fname[MAX_SYMLEN];
        if (namelen == 0 || namelen >= MAX_SYMLEN) fail("malformed call: missing function name");
        memcpy(fname, name_start, namelen);
        fname[namelen] = '\0';

        func_sig_t *sig = find_func_sig(fname);
        if (!sig) failf("call to undeclared function '%s' (functions must be declared with '@name(...) -> rN:' before they're called)", fname);

        const char *close_paren = strchr(paren_start, ')');
        if (!close_paren) fail("malformed call: missing ')'");
        strncpy(buf, paren_start + 1, (size_t)(close_paren - paren_start - 1));
        buf[close_paren - paren_start - 1] = '\0';

        operand_t argvals[MAX_PARAMS];
        int nargs = 0;
        char *tok = strtok(buf, ",");
        while (tok) {
            char *a = trim(tok);
            if (*a != '\0') {
                if (nargs >= MAX_PARAMS) fail("too many call arguments (max 12, matching r1-r12)");
                parse__operand(a, &argvals[nargs]);
                nargs++;
            }
            tok = strtok(NULL, ",");
        }
        if (nargs != sig->nparams) failf("wrong number of arguments in call to '%s' (see its matching '@name(...)' declaration)", fname);

        for (int a = 0; a < nargs; a++) {
            instr_t mv; memset(&mv, 0, sizeof(mv));
            mv.op = is_mem_operand(argvals[a].kind) ? OP_LOAD : OP_MOV;
            mv.src = argvals[a];
            mv.dst.kind = OPND_REG;
            mv.dst.reg_num = a + 1;
            push__instr(mv);
        }

        instr_t c; memset(&c, 0, sizeof(c));
        c.op = OP_CALL;
        c.dst.kind = OPND_LABEL;
        strncpy(c.dst.sym, fname, MAX_SYMLEN - 1);
        push__instr(c);

        /* Optional '> rX' after the call captures the return value: a
           plain register-to-register move out of the function's
           declared return register, skipped entirely if the caller
           discards the result (bare 'call f(args);' with nothing after
           it) or if rX is already the return register itself. */
        const char *arrow_gt = strchr(close_paren, '>');
        if (arrow_gt) {
            char destbuf[32];
            const char *ds = arrow_gt + 1;
            while (*ds == ' ') ds++;
            const char *de = strchr(ds, ';');
            if (!de) fail("malformed call: missing ';'");
            size_t dl = (size_t)(de - ds);
            if (dl == 0 || dl >= sizeof(destbuf)) fail("malformed call: expected a register after '>'");
            memcpy(destbuf, ds, dl);
            destbuf[dl] = '\0';
            operand_t destop;
            parse__operand(destbuf, &destop);
            if (destop.kind != OPND_REG || destop.is_sp) fail("call result destination must be a register (e.g. > r6), not sp or anything else");
            if (destop.reg_num != sig->ret_reg) {
                instr_t mv; memset(&mv, 0, sizeof(mv));
                mv.op = OP_MOV;
                mv.src.kind = OPND_REG;
                mv.src.reg_num = sig->ret_reg;
                mv.dst = destop;
                push__instr(mv);
            }
        }
        return 1;
    }

    /* jump-family: jmp/je/jne/jg/jl/call @LABEL;  -- or, for jmp/call
       only, an indirect register target: jmp rN; / call rN; jumps to
       the address held in rN rather than a fixed label (computed/
       indirect jump). Restricted to jmp/call: the conditional
       jN/call-family ops (je, jg, ...) express "branch on a flag to a
       fixed place", and none of the three target ISAs have a single
       conditional-and-indirect branch instruction -- doing that
       correctly needs an inverted-branch-around-indirect-jump sequence,
       a distinct enough feature that it isn't bundled in here silently. */
    for (size_t k = 0; k < N_JUMP_OP; k++) {
        if (strcmp(tokens[0], jump_ops[k].name) == 0) {
            if (ntok < 2) failf("'%s' requires a label or register operand", jump_ops[k].name);
            char target[MAX_SYMLEN];
            strncpy(target, tokens[1], MAX_SYMLEN - 1);
            target[MAX_SYMLEN - 1] = '\0';
            strip__semicolon(target);
            char *t = target;

            instr_t i; memset(&i, 0, sizeof(i));
            i.op = jump_ops[k].op;

            operand_t regop;
            if ((i.op == OP_JMP || i.op == OP_CALL) && parse__register(t, &regop)) {
                if (regop.is_sp) failf("'%s' cannot take 'sp' as an indirect target -- only r1-r12 hold addresses that make sense to jump to", jump_ops[k].name);
                if (regop.is_float) fail_fmt("'%s' cannot take a float register ('%s') as an indirect target", jump_ops[k].name, t);
                i.dst = regop;
                push__instr(i);
                return 1;
            }

            if (*t == '@') t++;
            if (*t == '\0') failf("'%s' requires a label or register operand", jump_ops[k].name);
            i.dst.kind = OPND_LABEL;
            strncpy(i.dst.sym, t, MAX_SYMLEN - 1);
            push__instr(i);
            return 1;
        }
    }

    /* three-operand arithmetic: op A, B > dst;  (dst = A OP B)
     *
     * Only makes sense for the commutative-shaped binary ops (add, sub,
     * mul, div, mod, and, or, xor, shl, shr, cmp) -- mov/load/store/lea
     * are inherently single-source and don't get a 3-operand form.
     * Chard v1 has no independent 3-operand instruction encoding on any
     * of its three targets (every backend's OP_ADD/SUB/etc. case emits
     * a strictly destructive 'dst = dst OP src'), so this is a
     * parse-time desugaring, not a new IR opcode: it lowers to
     *   mov A > dst;      (seed dst with the first operand)
     *   op  B > dst;       (apply the existing destructive 2-operand op)
     * reusing 100% of the tested per-target codegen for OP_MOV and the
     * binop itself. This means dst may safely alias A or B (including
     * dst == A == B): the seed happens before the binop reads dst, so
     * aliasing dst with either source is just the identity case falling
     * out naturally, the same way it would in the desugared code if a
     * human wrote it by hand.
     *
     * Detected by tokens[1] ending in ',' (tokenize() only splits on
     * whitespace, so 'r1,' arrives glued together as one token when the
     * source line is 'add r1, r2 > r3;') AND a '>' present somewhere
     * after it -- the '>' is what actually distinguishes this
     * 3-operand 'A, B > dst' form from the 2-operand comma form further
     * below ('+ src, dst;', no '>' anywhere), which also has tokens[1]
     * ending in ','. Without checking for '>' here too, '+ r1, r5;'
     * would wrongly fall into this block (tokens[1] = "r1," ends in a
     * comma) and fail with a confusing "expects 'A, B > dst'" error
     * instead of ever reaching the 2-operand comma parser. */
    if (ntok >= 2) {
        size_t t1len = strlen(tokens[1]);
        int has_comma_operand = (t1len > 0 && tokens[1][t1len - 1] == ',');
        int has_gt = 0;
        for (int ti = 2; ti < ntok; ti++) if (strcmp(tokens[ti], ">") == 0) { has_gt = 1; break; }
        if (has_comma_operand && has_gt) {
            for (size_t k = 0; k < N_TWO_OP; k++) {
                if (strcmp(tokens[0], two_operand_ops[k].name) != 0) continue;
                if (two_operand_ops[k].op == OP_MOV || two_operand_ops[k].op == OP_LOAD ||
                    two_operand_ops[k].op == OP_STORE || two_operand_ops[k].op == OP_LEA ||
                    two_operand_ops[k].op == OP_NOT || two_operand_ops[k].op == OP_NEG ||
                    two_operand_ops[k].op == OP_POPCOUNT || two_operand_ops[k].op == OP_CLZ ||
                    two_operand_ops[k].op == OP_CTZ)
                    failf("'%s' has no 3-operand form (op A, B > dst)", two_operand_ops[k].name);

                if (ntok < 5 || strcmp(tokens[3], ">") != 0)
                    failf("'%s' expects 'A, B > dst' operand syntax", two_operand_ops[k].name);

                char atok[MAX_SYMLEN], btok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
                strncpy(atok, tokens[1], MAX_SYMLEN - 1); atok[MAX_SYMLEN - 1] = '\0';
                atok[strlen(atok) - 1] = '\0'; /* strip trailing ',' */
                strncpy(btok, tokens[2], MAX_SYMLEN - 1); btok[MAX_SYMLEN - 1] = '\0';
                strncpy(dsttok, tokens[4], MAX_SYMLEN - 1); dsttok[MAX_SYMLEN - 1] = '\0';
                strip__semicolon(dsttok);
                if (*atok == '\0') fail("malformed 3-operand form: empty first operand before ','");

                operand_t aop, bop, dop;
                parse__operand(atok, &aop);
                parse__operand(btok, &bop);
                parse__operand(dsttok, &dop);
                if (dop.kind != OPND_REG)
                    failf("'%s' requires a register as its destination (op A, B > rX)", two_operand_ops[k].name);

                /* dst = A OP B, desugared as 'mv A > dst; op B > dst;' --
                 * but if dst aliases B (not just A), seeding with A first
                 * would clobber dst before B is ever read out of it,
                 * corrupting the result (e.g. 'add 1, r1 > r1;' seeding
                 * r1 with 1 before reading the old r1 as B). Whichever
                 * operand equals dst becomes the one read AFTER the seed,
                 * by simply seeding with the OTHER operand and computing
                 * with whichever one aliases dst (or with B by default
                 * when neither aliases dst, matching the order the user
                 * wrote). If both A and B alias dst, the seed is a no-op
                 * either way and any order is correct. */
                int b_aliases_dst = (bop.kind == OPND_REG && dop.kind == OPND_REG &&
                                      bop.is_sp == dop.is_sp && bop.reg_num == dop.reg_num);
                int a_aliases_dst = (aop.kind == OPND_REG && dop.kind == OPND_REG &&
                                      aop.is_sp == dop.is_sp && aop.reg_num == dop.reg_num);

                operand_t seed_src, compute_src;
                if (a_aliases_dst && !b_aliases_dst) {
                    /* dst already holds A's value; seed with B instead,
                       then apply A -- but the op itself isn't always
                       commutative (sub/div/mod/shl/shr), so this is only
                       safe when dst==A, which is the normal
                       already-supported case: seed with A (a no-op copy
                       of dst onto itself) and compute with B, unchanged
                       from before. */
                    seed_src = aop;
                    compute_src = bop;
                } else if (b_aliases_dst) {
                    /* dst aliases B (with or without also aliasing A):
                       seed with A so dst holds A's value, then compute
                       B OP-into-dst would read the NEW value, not the
                       old B -- so instead seed with B first (a no-op,
                       dst already equals B) and compute with A. This
                       changes operand order for non-commutative ops
                       (sub/div/mod/shl/shr), so swap which side of the
                       destructive op supplies the "other" operand only
                       when it's safe: since the destructive op is always
                       'dst = dst OP X', and we need 'dst = A OP B' with
                       dst==B, that's 'dst = A OP dst' -- not expressible
                       as 'dst = dst OP X' unless OP is commutative. For
                       non-commutative ops with dst==B, there is no valid
                       two-step desugar using only the existing
                       destructive primitive, so this is rejected below
                       rather than silently computing the wrong thing. */
                    seed_src = bop;
                    compute_src = aop;
                } else {
                    seed_src = aop;
                    compute_src = bop;
                }

                int op_is_commutative;
                switch (two_operand_ops[k].op) {
                    case OP_ADD: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR:
                        op_is_commutative = 1; break;
                    default:
                        op_is_commutative = 0; break;
                }
                if (b_aliases_dst && !a_aliases_dst && !op_is_commutative) {
                    char msg[192];
                    snprintf(msg, sizeof(msg),
                             "'%s' can't have its destination alias its second operand "
                             "(dst = A %s dst has no valid form here; assign to a different "
                             "register, or write it as the equivalent two-operand form)",
                             two_operand_ops[k].name, two_operand_ops[k].name);
                    fail(msg);
                }

                instr_t seed; memset(&seed, 0, sizeof(seed));
                seed.op = is_mem_operand(seed_src.kind) ? OP_LOAD : OP_MOV;
                seed.src = seed_src;
                seed.dst = dop;
                push__instr(seed);

                instr_t compute; memset(&compute, 0, sizeof(compute));
                compute.op = two_operand_ops[k].op;
                compute.src = compute_src;
                compute.dst = dop;
                push__instr(compute);
                return 1;
            }
            /* tokens[0] ended in a comma-bearing operand but wasn't a
               recognized binop -- fall through to the normal two-operand
               loop below, which will report the standard "unrecognized"
               style error via its own name match (finds nothing, falls
               through to 'return 0' at the end of this function). */
        }
    }

    /* two-operand, comma form: op src, dst;  (dst = dst OP src)
     *
     * A second spelling of the exact same 'op src > dst;' grammar just
     * below -- same operand order (src first, dst second -- 'dst' is
     * whichever operand comes AFTER the comma, mirroring 'dst' coming
     * after '>' in the word form), same opcode, same destructive
     * 'dst = dst OP src' semantics, same register-only destination
     * requirement. This is purely a delimiter swap (',' instead of
     * '>'), not a new operand order or a new IR shape -- both forms
     * build the exact same instr_t and go through the exact same
     * codegen. Exists because '+ r1, r5;' (or 'add r1, r5;') was felt
     * to read more like a real assembly instruction than 'add r1 > r5;'
     * does (the '>' can look like a plain move at a glance); '>' isn't
     * being removed, this is additive.
     *
     * Detected the same way the 3-operand form above is (tokens[1]
     * ending in ','), but crucially WITHOUT a '>' present anywhere
     * after it -- that's what routes 'add r1, r2 > r3;' into the
     * 3-operand block above while 'add r1, r5;' falls through to here
     * instead. Only two operands are allowed: a third comma-separated
     * token here is a real error (most likely a stray 3-operand form
     * missing its '> dst'), not silently reinterpreted. */
    if (ntok >= 2) {
        size_t t1len = strlen(tokens[1]);
        int has_comma_operand = (t1len > 0 && tokens[1][t1len - 1] == ',');
        int has_gt = 0;
        for (int ti = 2; ti < ntok; ti++) if (strcmp(tokens[ti], ">") == 0) { has_gt = 1; break; }
        if (has_comma_operand && !has_gt) {
            for (size_t k = 0; k < N_TWO_OP; k++) {
                if (strcmp(tokens[0], two_operand_ops[k].name) != 0) continue;

                if (ntok != 3)
                    failf("'%s' expects 'src, dst;' operand syntax (exactly two operands)", two_operand_ops[k].name);

                char srctok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
                strncpy(srctok, tokens[1], MAX_SYMLEN - 1); srctok[MAX_SYMLEN - 1] = '\0';
                srctok[strlen(srctok) - 1] = '\0'; /* strip trailing ',' */
                strncpy(dsttok, tokens[2], MAX_SYMLEN - 1); dsttok[MAX_SYMLEN - 1] = '\0';
                strip__semicolon(dsttok);
                if (*srctok == '\0') fail("malformed 'src, dst' form: empty first operand before ','");

                instr_t i; memset(&i, 0, sizeof(i));
                i.op = two_operand_ops[k].op;
                parse__operand(srctok, &i.src);
                parse__operand(dsttok, &i.dst);
                if (i.dst.kind != OPND_REG)
                    failf("'%s' requires a register as its destination (op src, rX; use 'store' to write to a symbol)", two_operand_ops[k].name);
                push__instr(i);
                return 1;
            }
            /* tokens[0] ended in a comma-bearing operand but wasn't a
               recognized binop -- fall through to the normal two-operand
               loop below, same reasoning as the 3-operand block's
               identical fallthrough above. */
        }
    }

    /* two-operand: op src > dst; or op dst < src; (register-only
       destination; a memory destination uses the fused
       DST(*rX) := rX(EXPR); form instead, handled by parse_fused_store
       before this function is ever called for such lines)

       '>' is accepted here for both the word mnemonics (mov/load/
       store/lea/add/sub/.../not/neg) -- their long-standing, original
       spelling -- and the C-style symbolic aliases ('+'/'-'/'*'/etc),
       which get '>' back alongside ',': both spellings build the
       exact same instr_t and go through the exact same codegen (see
       is_symbolic_op_name's comment for the shared-opcode rationale),
       so there's no parsing reason to deny '>' to the symbols -- ','
       remains available too, this is additive, matching the comma
       form's own "not being removed, just adding a spelling" stance.

       '<' is the same grammar with the arrow's sense flipped: 'op
       tokens[1] < tokens[3];' means exactly what 'op tokens[3] >
       tokens[1];' would -- the two operand tokens stay in the order
       the programmer wrote them, only which one is src and which is
       dst reverses, mirroring how the arrow itself reads backwards.
       This reads naturally when the destination is the more important
       thing to put first -- 'r1 < 5;' ("r1 gets 5") vs. 'mv 5 > r1;'
       ("5 goes into r1") -- both spellings build the identical instr_t
       and go through the identical codegen below; '<' is purely an
       additional spelling, exactly as additive as '>' already is
       relative to ','. */
    for (size_t k = 0; k < N_TWO_OP; k++) {
        if (strcmp(tokens[0], two_operand_ops[k].name) == 0) {
            int is_symbol = is_symbolic_op_name(two_operand_ops[k].name);
            int has_gt_form = (ntok >= 4 && strcmp(tokens[2], ">") == 0);
            int has_lt_form = (ntok >= 4 && strcmp(tokens[2], "<") == 0);
            if (!has_gt_form && !has_lt_form) {
                /* 'mv'/'load'/'store'/'lea' have no comma spelling (see
                   the comma-form block above, which only ever fires for
                   the arithmetic/bitwise family), so their error stays
                   '>'/'<'-only; every word mnemonic and every symbolic
                   alias in the arithmetic/bitwise family accepts both
                   comma and arrow spellings, so its error mentions all
                   three. */
                if (is_symbol || two_op_has_comma_form(two_operand_ops[k].op))
                    failf("'%s' expects 'src, dst;' operand syntax (or 'src > dst;' / 'dst < src;')", two_operand_ops[k].name);
                else
                    failf("'%s' expects 'src > dst' operand syntax (or 'dst < src;')", two_operand_ops[k].name);
            }
            char lhstok[MAX_SYMLEN], rhstok[MAX_SYMLEN];
            strncpy(lhstok, tokens[1], MAX_SYMLEN - 1);
            lhstok[MAX_SYMLEN - 1] = '\0';
            strncpy(rhstok, tokens[3], MAX_SYMLEN - 1);
            rhstok[MAX_SYMLEN - 1] = '\0';
            strip__semicolon(rhstok);
            instr_t i; memset(&i, 0, sizeof(i));
            i.op = two_operand_ops[k].op;
            if (has_lt_form) {
                /* 'op lhs < rhs;' -- arrow points left, so the operand
                   AFTER it (rhs) is the source and the operand BEFORE
                   it (lhs) is the destination: the exact mirror of the
                   '>' case just below, tokens left in the order written. */
                parse__operand(rhstok, &i.src);
                parse__operand(lhstok, &i.dst);
            } else {
                parse__operand(lhstok, &i.src);
                parse__operand(rhstok, &i.dst);
            }
            /* 'mv &SYM > rX;' / 'mv &local > rX;' -- address-of used
               to only be meaningful inside a syscall(...)/libcall(...)
               argument list (see check_addr_of_violations); this widens
               it to a general-purpose operand by desugaring straight
               into the existing 'lea SYM > rX;' opcode, which already
               does exactly this computation (materialize the operand's
               address into a register) on all three backends. Converting
               here, before the OP_LEA branch below runs, means the
               desugared instruction gets the exact same validation
               'lea' itself gets (symbol/local source, register
               destination, no bare '[ADDR]') for free, and codegen
               never has to know '&' was involved at all -- by the time
               it sees this instruction it's indistinguishable from one
               written as 'lea SYM > rX;' directly. is_addr_of is cleared
               afterward since OP_LEA computes an address unconditionally
               (that's the whole opcode, not a flag on it) -- leaving it
               set would trip check_addr_of_violations' blanket rejection
               of '&' outside syscall/libcall args, which still applies
               to every opcode that didn't just get rewritten away. */
            if (i.op == OP_MOV && i.src.is_addr_of) {
                i.op = OP_LEA;
                i.src.is_addr_of = 0;
            }
            if (i.op == OP_LEA) {
                if (!is_mem_operand(i.src.kind)) fail("'lea' requires a symbol as its source (lea SYM > rX)");
                if (i.src.kind == OPND_ADDR) fail("'lea' on a bare '[ADDR]' makes no sense -- the address is already a constant, just 'mv' it directly (mv 0xB8000 > rX)");
                if (i.dst.kind != OPND_REG) fail("'lea' requires a register as its destination (lea SYM > rX)");
            } else if (i.op == OP_STORE) {
                /* store rSRC > SYM; -- dst is deliberately a symbol
                   (global/local), the one exception in this family;
                   src must be the register being stored. Note: does
                   NOT accept '[ADDR]' here -- unlike SYM/local (sized
                   from decls[]/local_size), a bare address has no width
                   to infer, so it requires storeN (see the storeN block
                   above parse_instr_line's iloadN section), not plain
                   'store'. */
                if (i.src.kind != OPND_REG) fail("'store' requires a register as its source (store rX > SYM)");
                if (i.dst.kind == OPND_ADDR) fail("'store' to a bare '[ADDR]' needs a size -- use storeN instead (e.g. store2 rX > [0xB8000])");
                if (!is_mem_operand(i.dst.kind)) fail("'store' requires a symbol as its destination (store rX > SYM)");
            } else {
                /* mov/load/cmp/add/sub/mul/div/mod/and/or/xor/shl/shr
                   all require a register destination -- codegen for
                   every one of these calls reg__name(dst) with no
                   memory-operand fallback, so a local/global symbol
                   here would previously reach codegen unchecked and
                   silently emit a garbage operand (e.g. 'mv (null),
                   0') instead of failing at compile time. 'store' is
                   the only two-operand op whose destination is a
                   symbol, and it's handled separately above. */
                if (i.dst.kind != OPND_REG)
                    failf("'%s' requires a register as its destination (op src > rX; use 'store' to write to a symbol)", two_operand_ops[k].name);
                /* mv SYM > rX; / mv local > rX; -- 'mv' has no defined
                   "read this memory symbol" form anywhere else in the
                   compiler (see the fused-store comment above, which
                   substitutes OP_LOAD instead of OP_MOV specifically
                   because of this); a memory-operand source used to
                   reach OP_MOV's codegen unchecked and silently emit
                   garbage (reg__name() called on a non-register operand,
                   surfacing as literal '(null)' in the output). Reject
                   it here with a message pointing at the instruction
                   that actually has this form, the same way 'lea'/
                   'store' above are each restricted to their one
                   defined shape. */
                if (i.op == OP_MOV && i.src.kind == OPND_ADDR)
                    fail("'mv' cannot read a bare '[ADDR]' -- use loadN instead (e.g. load2 [0xB8000] > rX)");
                if (i.op == OP_MOV && is_mem_operand(i.src.kind))
                    fail("'mv' cannot read a symbol or local -- use 'load' instead (load SYM > rX)");
            }
            push__instr(i);
            return 1;
        }
    }

    /* float instructions: fop src > dst; -- same 'src > dst' syntax as
       the integer two-operand form above, with per-opcode operand-kind
       validation (a float op's register operands must actually be
       float registers, not integer ones, since the two namespaces
       don't auto-convert -- see OP_I2F/OP_F2I for the explicit
       conversion ops) done here rather than in codegen, so a mistake
       like 'fadd r1 > f2;' is rejected at compile time with a clear
       message instead of silently emitting whatever garbage falls out
       of treating an integer register as a float one. */
    for (size_t k = 0; k < N_FLOAT_OP; k++) {        if (strcmp(tokens[0], float_ops[k].name) != 0) continue;
        g_uses_float = 1;
        if (ntok < 4 || strcmp(tokens[2], ">") != 0) {
            failf("'%s' expects 'src > dst' operand syntax", float_ops[k].name);
        }
        char dsttok[MAX_SYMLEN];
        strncpy(dsttok, tokens[3], MAX_SYMLEN - 1);
        dsttok[MAX_SYMLEN - 1] = '\0';
        strip__semicolon(dsttok);
        instr_t i; memset(&i, 0, sizeof(i));
        i.op = float_ops[k].op;
        parse__operand(tokens[1], &i.src);
        parse__operand(dsttok, &i.dst);

        switch (i.op) {
        case OP_FMOV:
            if (i.src.kind == OPND_REG && !i.src.is_float) fail("'fmov' source register must be a float register (fN), not an integer one (rN)");
            if (i.src.kind == OPND_IMM && !i.src.is_float) fail("'fmov' requires a float-literal immediate (e.g. 3.14), not an integer one -- use 'i2f' to convert an integer register");
            if (i.dst.kind != OPND_REG || !i.dst.is_float) fail("'fmov' requires a float register as its destination (fmov SRC > fX)");
            /* src and dst must be the same register file (both s1-s8
               or both f1-f8) when src is itself a register -- same
               "fail loudly rather than silently do the wrong-width
               thing" reasoning as the FADD-family src-vs-dst check.
               fmov is a raw bit-pattern move (movss/movsd on x86-64,
               fmov s/d on AArch64, fmv.w.x/fmv.d.x-and-back on
               RISC-V), not a value-converting cvtsd2ss-style
               instruction, so 'fmov f1 > s1' would otherwise silently
               reinterpret f1's low 32 bits as an f32 bit pattern
               instead of actually converting the f64 value -- the
               same class of bug the FLOAD/FSTORE width-mismatch check
               above already guards against for symbol widths. */
            if (i.src.kind == OPND_REG && i.src.is_f32 != i.dst.is_f32)
                fail("'fmov' cannot mix register files -- src and dst must both be s-registers (f32) or both f-registers (f64); fmov moves raw bits; it does not convert precision");
            break;
        case OP_FLOAD:
            if (!is_mem_operand(i.src.kind)) fail("'fload' requires a symbol or local as its source (fload SYM > fX)");
            if (i.dst.kind != OPND_REG || !i.dst.is_float) fail("'fload' requires a float register as its destination (fload SYM > fX)");
            /* Register file (s1-s8 vs f1-f8) must match the symbol's
               declared width exactly -- no silent narrow or widen
               across a mismatch. An s-register load-and-stay-narrow
               only makes sense against an f32-declared symbol; f64
               storage into an s-register (or vice versa) is a real
               width mismatch, not something to guess about, same
               "fail loudly rather than silently do something the
               programmer didn't ask for" convention used elsewhere in
               this file (see e.g. the pin-collision checks). */
            {
                int sz = operand_mem_size(&i.src);
                if (i.dst.is_f32 && sz != 4)
                    failf("'fload' into an s-register (f32) requires an f32-declared source, but '%s' is declared f64 -- use an f-register (fN) instead, or redeclare the symbol as f32", i.src.sym);
                if (!i.dst.is_f32 && sz != 8)
                    failf("'fload' into an f-register (f64) requires an f64-declared source, but '%s' is declared f32 -- use an s-register (sN) instead, or redeclare the symbol as f64", i.src.sym);
            }
            break;
        case OP_FSTORE:
            if (i.src.kind != OPND_REG || !i.src.is_float) fail("'fstore' requires a float register as its source (fstore fX > SYM)");
            if (!is_mem_operand(i.dst.kind)) fail("'fstore' requires a symbol or local as its destination (fstore fX > SYM)");
            /* Mirrors OP_FLOAD's check above. */
            {
                int sz = operand_mem_size(&i.dst);
                if (i.src.is_f32 && sz != 4)
                    failf("'fstore' from an s-register (f32) requires an f32-declared destination, but '%s' is declared f64 -- use an f-register (fN) instead, or redeclare the symbol as f32", i.dst.sym);
                if (!i.src.is_f32 && sz != 8)
                    failf("'fstore' from an f-register (f64) requires an f64-declared destination, but '%s' is declared f32 -- use an s-register (sN) instead, or redeclare the symbol as f64", i.dst.sym);
            }
            break;
        case OP_VLOAD:
            /* Unlike fload, SYM here must name a whole two-element f64
               array (declared 'local/bss/volatile f64 name[2];'), not
               an ordinary scalar symbol -- vload reads 16 bytes as one
               unit (see the OP_VLOAD opcode_t comment for why: it's
               the only way to get two real f64 values into one vN
               register in a single instruction). An indexed access
               like 'name[i]' is rejected here too, same as a bare
               scalar symbol would be -- 'name[i]' resolves through
               OP_LALOAD's own machinery to a single element, not the
               array as a whole, so it carries the wrong shape for this
               op the same way a plain f64 symbol does. This is a
               shape check, not a width check like FLOAD's -- an
               unqualified '%s' error names the actual problem (wrong
               kind of symbol) rather than reusing FLOAD's width-
               mismatch phrasing, which would be misleading here. */
            if (!is_mem_operand(i.src.kind)) fail("'vload' requires a symbol or local as its source (vload SYM > fX)");
            if (i.dst.kind != OPND_REG || !i.dst.is_float) fail("'vload' requires a float register as its destination (vload SYM > fX)");
            {
                decl_t *d = find__decl(i.src.sym);
                if (!d || !d->is_float || d->size_bytes != 8 || d->array_len != 2)
                    failf("'vload' requires SYM to be a two-element f64 array (declared 'local/bss/volatile f64 %s[2];') -- it reads 16 bytes (two packed f64 lanes) as one unit, not a single scalar value", i.src.sym);
            }
            break;
        case OP_VSTORE:
            /* Mirrors OP_VLOAD's check above -- see it for the full
               rationale. */
            if (i.src.kind != OPND_REG || !i.src.is_float) fail("'vstore' requires a float register as its source (vstore fX > SYM)");
            if (!is_mem_operand(i.dst.kind)) fail("'vstore' requires a symbol or local as its destination (vstore fX > SYM)");
            {
                decl_t *d = find__decl(i.dst.sym);
                if (!d || !d->is_float || d->size_bytes != 8 || d->array_len != 2)
                    failf("'vstore' requires SYM to be a two-element f64 array (declared 'local/bss/volatile f64 %s[2];') -- it writes 16 bytes (two packed f64 lanes) as one unit, not a single scalar value", i.dst.sym);
            }
            break;
        case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: case OP_FCMP:
        case OP_FMIN: case OP_FMAX:
            if (i.dst.kind != OPND_REG || !i.dst.is_float) failf("'%s' requires a float register as its destination (op fSRC > fX)", float_ops[k].name);
            if (!((i.src.kind == OPND_REG && i.src.is_float) || (i.src.kind == OPND_IMM && i.src.is_float)))
                failf("'%s' requires a float register or float-literal source", float_ops[k].name);
            /* src and dst must be the same register file (both s1-s8
               or both f1-f8) -- an immediate has no file of its own
               (it's materialized directly into whichever file dst is,
               see each backend's codegen), so this only applies when
               src is itself a register. Same "fail loudly rather than
               silently do the wrong-width thing" reasoning as the
               FLOAD/FSTORE width-mismatch check above -- mixing files
               in one op (e.g. 'fadd s1 > f1') would otherwise silently
               read garbage bits across the width boundary. */
            if (i.src.kind == OPND_REG && i.src.is_f32 != i.dst.is_f32)
                failf("'%s' cannot mix register files -- src and dst must both be s-registers (f32) or both f-registers (f64)", float_ops[k].name);
            break;
        case OP_VADD: case OP_VSUB: case OP_VMUL: case OP_VDIV:
        case OP_VMIN: case OP_VMAX:
            /* Destructive two-operand like fadd, but register-only: a
               packed 2x-f64 op has no single-float literal spelling
               (see the OP_VADD opcode_t comment), so unlike fadd/fsub/
               fmul/fdiv an immediate source is rejected here rather
               than silently broadcast into both lanes. Same rule for
               all six vadd/vsub/vmul/vdiv/vmin/vmax -- none of them
               have a literal form. */
            if (i.dst.kind != OPND_REG || !i.dst.is_float) failf("'%s' requires a float register as its destination (op fSRC > fX)", float_ops[k].name);
            if (i.src.kind != OPND_REG || !i.src.is_float) failf("'%s' requires a float register as its source -- no float-literal immediate (a packed 2x-f64 op has no single-float literal form)", float_ops[k].name);
            break;
        case OP_VSQRT: case OP_VABS: case OP_VNEG: case OP_VDUP:
            /* Unary like fsqrt/fabs/fneg (src is the whole input, no
               dst-as-input combine step), but register-only like the
               rest of the vN family (no float-literal source -- see
               OP_VADD's rejection just above; a packed op has no
               single-float literal spelling whether it's unary or
               destructive two-operand). src and dst may be the same
               register (in-place), same as fsqrt/fabs/fneg. vdup
               additionally only reads src's low lane (the other three
               read both), but that's a codegen distinction, not an
               operand-shape one -- the same rule applies here. */
            if (i.dst.kind != OPND_REG || !i.dst.is_float) failf("'%s' requires a float register as its destination (op fSRC > fX)", float_ops[k].name);
            if (i.src.kind != OPND_REG || !i.src.is_float) failf("'%s' requires a float register as its source -- no float-literal immediate (a packed 2x-f64 op has no single-float literal form)", float_ops[k].name);
            break;
        case OP_FSQRT: case OP_FABS: case OP_FNEG:
            /* Unary: src is read, dst is written, same 'src > dst' shape
               as fmov -- no float-literal source allowed (unlike fadd/
               fsub/fmul/fdiv/fmin/fmax, which combine dst with src; here
               src IS the entire input, so a literal source is just a
               compile-time-constant result the caller could compute
               themselves rather than something worth a codegen path
               for). src and dst may be the same register (in-place). */
            if (i.src.kind != OPND_REG || !i.src.is_float) failf("'%s' requires a float register as its source (op fSRC > fX)", float_ops[k].name);
            if (i.dst.kind != OPND_REG || !i.dst.is_float) failf("'%s' requires a float register as its destination (op fSRC > fX)", float_ops[k].name);
            /* src and dst must be the same register file, same
               reasoning as the FADD-family src-vs-dst check and
               FMOV's own version of this check just above -- fsqrt/
               fabs/fneg are unary, dst-from-src, not two-source-combine
               ops, but codegen still picks the instruction's width
               purely from dst.is_f32 (see each backend's OP_FSQRT/
               OP_FABS/OP_FNEG case), so an unchecked 'fsqrt s1 > f2'
               would silently compute sqrtsd against a register whose
               low 32 bits hold a real value and whose high 32 bits
               hold whatever was last written there -- garbage, not
               zero. */
            if (i.src.is_f32 != i.dst.is_f32)
                failf("'%s' cannot mix register files -- src and dst must both be s-registers (f32) or both f-registers (f64)", float_ops[k].name);
            break;
        case OP_I2F:
            if (i.src.kind == OPND_REG && i.src.is_float) fail("'i2f' source must be an integer register (rN), not a float one -- it's already a float");
            if (i.dst.kind != OPND_REG || !i.dst.is_float) fail("'i2f' requires a float register as its destination (i2f rSRC > fX)");
            break;
        case OP_F2I:
            if (i.src.kind != OPND_REG || !i.src.is_float) fail("'f2i' requires a float register as its source (f2i fSRC > rX)");
            if (i.dst.kind != OPND_REG || i.dst.is_float) fail("'f2i' requires an integer register as its destination (f2i fSRC > rX)");
            break;
        default: break;
        }

        push__instr(i);
        return 1;
    }

    /* fma fA, fB, fC > fDST;  fDST = (fA * fB) + fC, one fused multiply-
       add with a single rounding step -- see the OP_FMA opcode_t comment
       for the full contract and why it needs its own parse block instead
       of joining float_ops above: three source operands don't fit that
       loop's shared 'src > dst' (one source) shape, the same reason
       'atom=' (SYM, rEXPECTED, rDESIRED > rDST) already has its own
       block below rather than joining the atomic_rmw_ops loop above it.
       All four operands must be float registers -- no float-literal
       immediate on any of fA/fB/fC (see the opcode_t comment for why),
       matching how atom='s rEXPECTED/rDESIRED are register-only too. */
    if (strcmp(tokens[0], "fma") == 0) {
        g_uses_float = 1;
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char atok[MAX_SYMLEN], btok[MAX_SYMLEN], ctok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63[^,], %63[^,], %63s > %63s", atok, btok, ctok, dsttok) != 4)
            fail("malformed 'fma': expected 'fma fA, fB, fC > fDST;'");
        strip__semicolon(dsttok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_FMA;
        /* cas_expected/result_reg/cas_desired reused as fA/fB/fC -- see
           the instr_t field comments and the OP_FMA opcode_t comment. */
        parse__operand(atok, &i.cas_expected);
        parse__operand(btok, &i.result_reg);
        parse__operand(ctok, &i.cas_desired);
        parse__operand(dsttok, &i.dst);
        if (i.cas_expected.kind != OPND_REG || !i.cas_expected.is_float) failf("'fma': fA must be a float register, not '%s'", atok);
        if (i.result_reg.kind != OPND_REG || !i.result_reg.is_float) failf("'fma': fB must be a float register, not '%s'", btok);
        if (i.cas_desired.kind != OPND_REG || !i.cas_desired.is_float) failf("'fma': fC must be a float register, not '%s'", ctok);
        if (i.dst.kind != OPND_REG || !i.dst.is_float) fail("'fma' requires a float register as its destination (fma fA, fB, fC > fX)");
        /* fA/fB/fC/dst must all share the same register file (all
           s1-s8 or all f1-f8) -- same "fail loudly rather than
           silently do the wrong-width thing" reasoning as the
           FADD-family src-vs-dst check above, just extended across
           three source operands instead of one. Codegen picks the
           narrow vs. wide instruction form purely off dst.is_f32 (see
           each backend's OP_FMA case), so an unchecked width mismatch
           here would silently read/write garbage bits across the
           width boundary on whichever of fA/fB/fC disagreed with
           dst -- the same class of bug this guards against
           elsewhere, just with three operands to check instead of
           one. */
        if (i.cas_expected.is_f32 != i.dst.is_f32)
            failf("'fma': fA ('%s') and fDST must be the same register file -- both s-registers (f32) or both f-registers (f64)", atok);
        if (i.result_reg.is_f32 != i.dst.is_f32)
            failf("'fma': fB ('%s') and fDST must be the same register file -- both s-registers (f32) or both f-registers (f64)", btok);
        if (i.cas_desired.is_f32 != i.dst.is_f32)
            failf("'fma': fC ('%s') and fDST must be the same register file -- both s-registers (f32) or both f-registers (f64)", ctok);
        push__instr(i);
        return 1;
    }

    /* vfma fA, fB, fC > fDST;  packed 2x-f64 fused multiply-add -- the
       vN family's counterpart to 'fma' just above (see the OP_VFMA
       opcode_t comment for the full contract). Same three-source shape
       as 'fma', so it needs the same dedicated parse block for the same
       reason ('src > dst' from float_ops' shared loop doesn't fit three
       sources) -- but register-only AND f64-only (f-registers, never
       s-registers), unlike 'fma' which allows f32 via s-registers.
       There's no vfma-of-s-registers the same way there's no
       vadd-of-s-registers: the whole vN family only operates on the
       f1-f8 128-bit-pair file (see OP_VADD's opcode_t comment). */
    if (strcmp(tokens[0], "vfma") == 0) {
        g_uses_float = 1;
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char atok[MAX_SYMLEN], btok[MAX_SYMLEN], ctok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63[^,], %63[^,], %63s > %63s", atok, btok, ctok, dsttok) != 4)
            fail("malformed 'vfma': expected 'vfma fA, fB, fC > fDST;'");
        strip__semicolon(dsttok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_VFMA;
        /* cas_expected/result_reg/cas_desired reused as fA/fB/fC, same
           as OP_FMA -- see the instr_t field comments and the OP_VFMA
           opcode_t comment. */
        parse__operand(atok, &i.cas_expected);
        parse__operand(btok, &i.result_reg);
        parse__operand(ctok, &i.cas_desired);
        parse__operand(dsttok, &i.dst);
        if (i.cas_expected.kind != OPND_REG || !i.cas_expected.is_float || i.cas_expected.is_f32)
            failf("'vfma': fA must be an f-register (f1-f8), not '%s' -- a packed 2x-f64 op has no s-register (f32) form", atok);
        if (i.result_reg.kind != OPND_REG || !i.result_reg.is_float || i.result_reg.is_f32)
            failf("'vfma': fB must be an f-register (f1-f8), not '%s' -- a packed 2x-f64 op has no s-register (f32) form", btok);
        if (i.cas_desired.kind != OPND_REG || !i.cas_desired.is_float || i.cas_desired.is_f32)
            failf("'vfma': fC must be an f-register (f1-f8), not '%s' -- a packed 2x-f64 op has no s-register (f32) form", ctok);
        if (i.dst.kind != OPND_REG || !i.dst.is_float || i.dst.is_f32)
            fail("'vfma' requires an f-register (f1-f8) as its destination -- a packed 2x-f64 op has no s-register (f32) form (vfma fA, fB, fC > fX)");
        push__instr(i);
        return 1;
    }

    /* vfms fA, fB, fC > fDST;  packed 2x-f64 fused multiply-subtract --
       sibling of 'vfma' just above (see the OP_VFMS opcode_t comment
       for the full contract: fDST = (fA*fB)-fC). Identical parse shape
       to 'vfma' (same three-source block, same f-register-only/
       f64-only restriction, same operand-order and error-message
       conventions) -- only the opcode and the mnemonic in error
       messages differ. */
    if (strcmp(tokens[0], "vfms") == 0) {
        g_uses_float = 1;
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char atok[MAX_SYMLEN], btok[MAX_SYMLEN], ctok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63[^,], %63[^,], %63s > %63s", atok, btok, ctok, dsttok) != 4)
            fail("malformed 'vfms': expected 'vfms fA, fB, fC > fDST;'");
        strip__semicolon(dsttok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_VFMS;
        /* cas_expected/result_reg/cas_desired reused as fA/fB/fC, same
           as OP_VFMA/OP_FMA -- see the instr_t field comments and the
           OP_VFMS opcode_t comment. */
        parse__operand(atok, &i.cas_expected);
        parse__operand(btok, &i.result_reg);
        parse__operand(ctok, &i.cas_desired);
        parse__operand(dsttok, &i.dst);
        if (i.cas_expected.kind != OPND_REG || !i.cas_expected.is_float || i.cas_expected.is_f32)
            failf("'vfms': fA must be an f-register (f1-f8), not '%s' -- a packed 2x-f64 op has no s-register (f32) form", atok);
        if (i.result_reg.kind != OPND_REG || !i.result_reg.is_float || i.result_reg.is_f32)
            failf("'vfms': fB must be an f-register (f1-f8), not '%s' -- a packed 2x-f64 op has no s-register (f32) form", btok);
        if (i.cas_desired.kind != OPND_REG || !i.cas_desired.is_float || i.cas_desired.is_f32)
            failf("'vfms': fC must be an f-register (f1-f8), not '%s' -- a packed 2x-f64 op has no s-register (f32) form", ctok);
        if (i.dst.kind != OPND_REG || !i.dst.is_float || i.dst.is_f32)
            fail("'vfms' requires an f-register (f1-f8) as its destination -- a packed 2x-f64 op has no s-register (f32) form (vfms fA, fB, fC > fX)");
        push__instr(i);
        return 1;
    }

    /* vfnma fA, fB, fC > fDST;  packed 2x-f64 fused negate-multiply-add
       -- sibling of 'vfma'/'vfms' above (see the OP_VFNMA opcode_t
       comment for the full contract: fDST = fC-(fA*fB)). Identical
       parse shape to 'vfma'/'vfms' -- only the opcode and the mnemonic
       in error messages differ. */
    if (strcmp(tokens[0], "vfnma") == 0) {
        g_uses_float = 1;
        char buf[MAX_LINE];
        strncpy(buf, raw_trimmed, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char atok[MAX_SYMLEN], btok[MAX_SYMLEN], ctok[MAX_SYMLEN], dsttok[MAX_SYMLEN];
        if (sscanf(buf, "%*s %63[^,], %63[^,], %63s > %63s", atok, btok, ctok, dsttok) != 4)
            fail("malformed 'vfnma': expected 'vfnma fA, fB, fC > fDST;'");
        strip__semicolon(dsttok);

        instr_t i; memset(&i, 0, sizeof(i));
        i.op = OP_VFNMA;
        /* cas_expected/result_reg/cas_desired reused as fA/fB/fC, same
           as OP_VFMA/OP_VFMS/OP_FMA -- see the instr_t field comments
           and the OP_VFNMA opcode_t comment. */
        parse__operand(atok, &i.cas_expected);
        parse__operand(btok, &i.result_reg);
        parse__operand(ctok, &i.cas_desired);
        parse__operand(dsttok, &i.dst);
        if (i.cas_expected.kind != OPND_REG || !i.cas_expected.is_float || i.cas_expected.is_f32)
            failf("'vfnma': fA must be an f-register (f1-f8), not '%s' -- a packed 2x-f64 op has no s-register (f32) form", atok);
        if (i.result_reg.kind != OPND_REG || !i.result_reg.is_float || i.result_reg.is_f32)
            failf("'vfnma': fB must be an f-register (f1-f8), not '%s' -- a packed 2x-f64 op has no s-register (f32) form", btok);
        if (i.cas_desired.kind != OPND_REG || !i.cas_desired.is_float || i.cas_desired.is_f32)
            failf("'vfnma': fC must be an f-register (f1-f8), not '%s' -- a packed 2x-f64 op has no s-register (f32) form", ctok);
        if (i.dst.kind != OPND_REG || !i.dst.is_float || i.dst.is_f32)
            fail("'vfnma' requires an f-register (f1-f8) as its destination -- a packed 2x-f64 op has no s-register (f32) form (vfnma fA, fB, fC > fX)");
        push__instr(i);
        return 1;
    }

    /* Implicit libc-call: 'NAME(args) [> rX];' with no leading
       'libc-call' keyword at all. This is now the ONLY valid spelling
       for calling a declared extern, except in the one collision case
       described below (see the matching comment above the 'libc-call'
       keyword's own parse site for the other half of this rule).
       Reachable only here, after every other keyword-shaped statement
       above has already had first crack at the line -- so this never
       shadows 'call NAME(...)' (Chard's own r1-r12-convention function
       calls, which keep requiring their own keyword), any directive, or
       anything else recognized earlier. Fires when:
         - the line looks like 'IDENT(' at all (cheapest check first,
           to fall through fast for the vast majority of lines that
           aren't calls of any kind),
         - IDENT is a plain identifier (letters/digits/underscore) --
           guards against accidentally matching a stray '(' that
           belongs to some other, unrelated malformed line and
           misattributing it to this fallback's error messages instead
           of whatever the real problem was,
         - libc-init has already run (same ordering requirement the
           explicit 'libc-call' keyword enforces -- calling into libc
           before its startup ran is unsafe regardless of which
           spelling was used to write the call),
         - IDENT is a declared extern, AND
         - IDENT is NOT also a Chard function name (find_func_sig) --
           if a program somehow declared both '@IDENT(...) -> rN' and
           'extern IDENT(n);' with the same name (legal today, if
           unusual: they occupy different lookup tables and don't
           collide with each other the way two externs or two struct
           fields would), a *bare* 'IDENT(...)' is genuinely ambiguous
           between "Chard function, forgot the 'call' keyword" and
           "libc function, using the no-keyword shorthand" -- in that
           specific case, and ONLY that case, this fallback backs off
           and falls through, requiring the explicit 'libc-call'
           keyword to disambiguate, rather than silently guessing. This
           is the sole surviving reason 'libc-call' still parses at
           all; everywhere else it's a hard error (see above).
       If IDENT doesn't satisfy every one of the above, this simply
       doesn't match and control falls through to 'return 0' below --
       it does NOT fail() on a near-miss, since a near-miss might
       legitimately be some other malformed statement that deserves
       its own, more specific error message instead of this one. */
    {
        char *paren = strchr(tokens[0], '(');
        if (paren && paren != tokens[0]) {
            size_t idlen = (size_t)(paren - tokens[0]);
            int looks_like_ident = (isalpha((unsigned char)tokens[0][0]) || tokens[0][0] == '_');
            for (size_t k = 1; looks_like_ident && k < idlen; k++)
                if (!isalnum((unsigned char)tokens[0][k]) && tokens[0][k] != '_') looks_like_ident = 0;
            if (looks_like_ident && idlen < MAX_SYMLEN) {
                char ident[MAX_SYMLEN];
                memcpy(ident, tokens[0], idlen);
                ident[idlen] = '\0';
                if (g_libc_init_seen && find_extern(ident) && !find_func_sig(ident)) {
                    parse_libc_call_body(raw_trimmed, "libc-call");
                    return 1;
                }
            }
        }
    }

    return 0; /* not recognized as an instruction */
}

