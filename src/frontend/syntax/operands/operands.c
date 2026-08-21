#include "../../../chard.h"

int is_mem_operand(opnd_kind_t k) { return k == OPND_SYM || k == OPND_LOCAL || k == OPND_ADDR; }

void parse__xaddr(const char *mnemonic, const char *raw_expr,
                          operand_t *out_idx, int *out_scale, long *out_disp) {
    char expr[MAX_LINE];
    strncpy(expr, raw_expr, sizeof(expr) - 1);
    expr[sizeof(expr) - 1] = '\0';

    /* Split on the first of '*' or '+'/'-' (a leading '-' would belong
       to a negative DISP with no explicit '+', e.g. 'rIDX-8'; only a
       '+'/'-' that isn't the very first character can be that
       separator, since the index register token itself never starts
       with '+'/'-'). */
    char *star = strchr(expr, '*');
    char *plus = strchr(expr + 1, '+'); /* +1: skip a possible leading sign, irrelevant here anyway */
    char *minus = strchr(expr + 1, '-');
    char *disp_sep = plus ? plus : minus;
    if (disp_sep && star && disp_sep < star) disp_sep = NULL; /* '+'/'-' before '*' would be malformed; let scale-then-disp order below catch it */

    char idxtok[MAX_SYMLEN];
    const char *idx_end = star ? star : (disp_sep ? disp_sep : expr + strlen(expr));
    size_t idxlen = (size_t)(idx_end - expr);
    if (idxlen == 0 || idxlen >= sizeof(idxtok)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: malformed index expression '%s'", mnemonic, raw_expr);
        fail(msg);
    }
    memcpy(idxtok, expr, idxlen);
    idxtok[idxlen] = '\0';
    if (!parse__register(trim(idxtok), out_idx)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: '%s' is not a valid index register", mnemonic, idxtok);
        fail(msg);
    }

    *out_scale = 1;
    *out_disp = 0;

    if (star) {
        char scaletok[32];
        const char *scale_end = disp_sep ? disp_sep : expr + strlen(expr);
        if (disp_sep && disp_sep < star) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s: malformed addressing expression '%s' (expected 'rIDX*SCALE+-DISP')", mnemonic, raw_expr);
            fail(msg);
        }
        size_t scalelen = (size_t)(scale_end - (star + 1));
        if (scalelen == 0 || scalelen >= sizeof(scaletok)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s: malformed scale in '%s'", mnemonic, raw_expr);
            fail(msg);
        }
        memcpy(scaletok, star + 1, scalelen);
        scaletok[scalelen] = '\0';
        char *st = trim(scaletok);
        if (!is__number(st)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s: scale must be a plain integer literal (got '%s')", mnemonic, st);
            fail(msg);
        }
        long sc = parse__number(st);
        if (sc != 1 && sc != 2 && sc != 4 && sc != 8) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s: scale must be 1, 2, 4, or 8 (got %ld)", mnemonic, sc);
            fail(msg);
        }
        *out_scale = (int)sc;
    }

    if (disp_sep) {
        char disptok[64];
        strncpy(disptok, disp_sep, sizeof(disptok) - 1); /* includes the leading +/- */
        disptok[sizeof(disptok) - 1] = '\0';
        char *dt = trim(disptok);
        /* is__number (like parse__number) only ever recognizes an
           optional leading '-', never '+' -- '+8' means the same
           thing as '8' but isn't itself a literal is__number accepts,
           so a leading '+' is stripped here before validating/parsing.
           A leading '-' is left alone; parse__number already handles
           negation. */
        if (*dt == '+') dt = trim(dt + 1);
        if (!is__number(dt)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s: displacement must be a plain integer literal (got '%s')", mnemonic, dt);
            fail(msg);
        }
        *out_disp = parse__number(dt);
    }
}

void parse__operand(const char *tok, operand_t *out) {
    memset(out, 0, sizeof(*out));
    /* '&SYM' / '&local' -- address-of, same spelling and same
       string-level stripping 'global rN = &SYM;' already established
       (see parse_global_pin) -- '&' never becomes its own token
       (tokenizer only splits on whitespace), so '&SYM'/'&local' arrives
       here as one token like any bare identifier. Checked first, ahead
       of every other operand form, since none of register/immediate/
       paren-expr/float-literal/param/local syntax can begin with '&'
       anyway -- so this just strips the sigil and lets the rest of the
       function run exactly as before on the bare name (which resolves
       against in-scope locals first, same as any other bare-name
       lookup, then falls back to OPND_SYM), then stamps the result
       afterward. Accepting OPND_LOCAL here (not just OPND_SYM) is what
       makes '&local' work: the actual address computation for a local
       -- frame-pointer-relative, possibly chasing frames_up saved-fp
       links -- already exists in every backend (x86_addr_text,
       aarch64_local_base, riscv_local_base) because OP_LEA and
       array-element addressing already need it; is_addr_of on an
       OPND_LOCAL just tells OP_SYSCALL's argument marshalling to reach
       that same existing code instead of doing a value load. */
    if (tok[0] == '&') {
        if (tok[1] == '\0') fail("'&' must be followed by a symbol name");
        parse__operand(tok + 1, out);
        if (out->kind != OPND_SYM && out->kind != OPND_LOCAL) {
            char msg[128];
            snprintf(msg, sizeof(msg), "'&%s': address-of only applies to a volatile/bss global symbol or a local", tok + 1);
            fail(msg);
        }
        out->is_addr_of = 1;
        return;
    }
    /* '[EXPR]' -- absolute memory address, a compile-time-constant
       integer (hex/decimal literal or paren-expr), never a register.
       A *register-held* address already has dedicated opcodes
       (iload/istore/xload/xstore rBASE[rIDX...]); this is specifically
       for addresses that are themselves fixed at compile time -- an
       MMIO register, a hardcoded VGA buffer, a fixed page-table
       location -- the class of thing bare-metal/bootloader code needs
       constantly and iload/xload's register-relative model doesn't
       cover, previously reachable only via the 'raw "...";' escape
       hatch (target-specific text, no portability, no validation).
       Checked ahead of every other form for the same reason '&' is:
       none of register/immediate/paren-expr/float/param/local syntax
       can begin with '[' anyway.
       Unlike OPND_SYM/OPND_LOCAL, an absolute address has no decls[]
       entry to size itself from, so local_size is left 0 here
       (unsized) -- the loadK/storeK opcode parse stamps in the real
       width from its own size suffix, exactly like OPND_LOCAL's width
       already comes from context rather than the operand token
       itself. */
    if (tok[0] == '[') {
        size_t tlen = strlen(tok);
        if (tlen < 2 || tok[tlen - 1] != ']') fail("'[EXPR]': missing closing ']'");
        char inner[MAX_LINE];
        size_t ilen = tlen - 2;
        if (ilen == 0) fail("'[EXPR]': empty address expression");
        if (ilen >= sizeof(inner)) fail("'[EXPR]': address expression too long");
        memcpy(inner, tok + 1, ilen);
        inner[ilen] = '\0';
        long addr;
        if (is__number(inner)) {
            addr = parse__number(inner);
        } else if (!try_parse_paren_expr(inner, &addr)) {
            fail("'[EXPR]': expected a constant integer address (e.g. [0xB8000])");
        }
        if (addr < 0) fail("'[EXPR]': address must be non-negative");
        out->kind = OPND_ADDR;
        out->imm = addr;
        return;
    }
    if (parse__register(tok, out)) return;
    if (is__number(tok)) {
        out->kind = OPND_IMM;
        out->imm = parse__number(tok);
        return;
    }
    {
        long ev;
        if (try_parse_paren_expr(tok, &ev)) {
            out->kind = OPND_IMM;
            out->imm = ev;
            return;
        }
    }
    if (is_float_literal(tok)) {
        out->kind = OPND_IMM;
        out->is_float = 1;
        out->fimm = atof(tok);
        return;
    }
    /* Function parameters resolve straight to a register (see the
       "Function parameters" section): 'a' in '@add(a, b) -> r1: { ... }'
       is just another name for r1, costing nothing at runtime, unlike a
       local (real stack storage) or a symbol (a load/store). Checked
       ahead of locals since a parameter is the "outermost" binding for
       a name within a function body. */
    int preg = find_param_reg(tok);
    if (preg > 0) {
        out->kind = OPND_REG;
        out->reg_num = preg;
        return;
    }
    /* bare symbol / label reference -- resolved against currently
       in-scope locals right now, at parse time, since a local's
       decls[] entry only exists between its block's '{' and '}' (see
       declare__local/close_local_frame). Capturing local_offset/
       local_size directly on the operand means codegen never needs to
       re-look the name up later, when the local may no longer be in
       decls[] at all. A volatile/bss symbol, or a forward reference to
       one not yet declared, stays OPND_SYM as before. */
    decl_t *ld = find__decl(tok);
    if (ld && ld->section == SEC_LOCAL) {
        out->kind = OPND_LOCAL;
        strncpy(out->sym, tok, MAX_SYMLEN - 1); /* diagnostics only */
        out->local_offset = ld->local_offset;
        out->local_size = ld->size_bytes;
        /* local_frame_depth is this reference's own current nesting
           depth (see open_local_frame/close_local_frame); ld->local_depth
           is the depth the block that declared this local was at. Their
           difference is how many saved-fp links codegen must walk at
           runtime to reach the frame the local actually lives in -- 0
           when the reference is in the same block (or a further-nested
           block hasn't been entered since), growing by 1 per enclosing
           @label block the reference sits inside relative to the
           declaration. */
        out->frames_up = local_frame_depth - ld->local_depth;
        return;
    }
    out->kind = OPND_SYM;
    strncpy(out->sym, tok, MAX_SYMLEN - 1);
}

void render_simple_operand(target_t t, operand_t *o, char *buf, size_t bufsz) {
    if (o->kind == OPND_REG) {
        snprintf(buf, bufsz, "%s", reg__name(t, o));
    } else if (o->kind == OPND_IMM) {
        snprintf(buf, bufsz, "%s%ld", target_defs[t].imm_prefix, o->imm);
    } else {
        buf[0] = '\0';
    }
}

void x86_addr_text(FILE *out, const operand_t *o, char *buf, size_t bufsz) {
    if (o->kind == OPND_LOCAL) {
        if (o->frames_up == 0) {
            snprintf(buf, bufsz, "rbp-%d", o->local_offset);
        } else {
            fprintf(out, "    mov %s, [rbp]\n", SCRATCH_X86);
            for (int i = 1; i < o->frames_up; i++)
                fprintf(out, "    mov %s, [%s]\n", SCRATCH_X86, SCRATCH_X86);
            snprintf(buf, bufsz, "%s-%d", SCRATCH_X86, o->local_offset);
        }
    } else if (o->kind == OPND_ADDR) {
        /* Absolute address, e.g. '[0xB8000]' -- NASM accepts a bare
           numeric literal as the memory operand directly, so this
           needs no register staging at all, unlike OPND_LOCAL's
           frames_up chase. */
        snprintf(buf, bufsz, "0x%lx", (unsigned long)o->imm);
    } else {
        snprintf(buf, bufsz, "%s", o->sym);
    }
}

int operand_mem_size(const operand_t *o) {
    if (o->kind == OPND_LOCAL) return o->local_size;
    if (o->kind == OPND_ADDR) return o->local_size; /* stamped by loadN/storeN parsing -- see that block's comment */
    decl_t *d = find__decl(o->sym);
    return d ? d->size_bytes : 8;
}

