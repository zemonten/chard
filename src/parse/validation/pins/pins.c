#include "../../../chard.h"

global_pin_t *find_pin_by_reg(int reg_num) {
    for (int i = 0; i < nglobal_pins; i++)
        if (global_pins[i].reg_num == reg_num) return &global_pins[i];
    return NULL;
}

global_pin_t *find_pin_by_sym(const char *sym) {
    for (int i = 0; i < nglobal_pins; i++)
        if (strcmp(global_pins[i].sym, sym) == 0) return &global_pins[i];
    return NULL;
}

int find_param_reg(const char *name) {
    if (!in_function) return 0;
    for (int i = 0; i < current_params.nparams; i++)
        if (strcmp(current_params.names[i], name) == 0) return i + 1;
    return 0;
}

int parse_global_pin(char *tokens[], int ntok, char *raw_line) {
    if (ntok < 1 || strcmp(tokens[0], "global") != 0) return 0;
    if (ntok < 4) fail("malformed 'global': expected 'global rN = SYM;' or 'global rN = &SYM;'");

    operand_t regop; memset(&regop, 0, sizeof(regop));
    if (!parse__register(tokens[1], &regop)) failf("global: '%s' is not a valid register", tokens[1]);
    if (regop.is_sp) fail("global: 'sp' cannot be pinned");
    if (regop.is_float) fail("global: only integer registers (r1-r12) can be pinned, not float registers");

    if (strcmp(tokens[2], "=") != 0) fail("malformed 'global': expected 'global rN = SYM;' or 'global rN = &SYM;'");

    char rhs[MAX_SYMLEN];
    strncpy(rhs, tokens[3], MAX_SYMLEN - 1);
    rhs[MAX_SYMLEN - 1] = '\0';
    strip__semicolon(rhs);

    /* 'global rN = &SYM;' -- address pin. The '&' is stripped here at
       the string level (never becomes its own token: Chard's tokenizer
       only splits on whitespace, see tokenize(), so '&SYM' arrives as
       one token like any bare identifier would) rather than teaching
       the tokenizer a new operator, since '&' has no other meaning
       anywhere else in the grammar (bitwise AND is spelled 'and', not
       '&' -- see OP_AND) and this is the only place address-of syntax
       exists in Chard v1. */
    int is_addr = (rhs[0] == '&');
    char *sym = is_addr ? rhs + 1 : rhs;
    if (*sym == '\0') fail("malformed 'global': expected a symbol after '=' (or '=&')");

    if (find_pin_by_reg(regop.reg_num)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "register r%d is already pinned to another symbol", regop.reg_num);
        fail(msg);
    }
    if (find_pin_by_sym(sym)) failf("global: symbol '%s' is already pinned to a register", sym);

    if (nglobal_pins >= MAX_GLOBAL_PINS) fail("too many pinned globals");
    global_pin_t *p = &global_pins[nglobal_pins++];
    p->reg_num = regop.reg_num;
    p->is_addr = is_addr;
    strncpy(p->sym, sym, MAX_SYMLEN - 1);

    (void)raw_line;
    return 1;
}

void splice_global_pin_loads(void) {
    if (nglobal_pins == 0) return;

    int entry_idx = -1;
    for (int i = 0; i < nprog; i++) {
        if (prog[i].op == OP_LABEL && prog[i].is_entry) { entry_idx = i; break; }
    }
    if (entry_idx < 0) fail("'global' pins require an entry block (@start { ... })");

    DA_ENSURE_N(prog, prog_cap, nprog + nglobal_pins, instr_t);

    /* shift everything after the entry label right by nglobal_pins */
    memmove(&prog[entry_idx + 1 + nglobal_pins], &prog[entry_idx + 1],
            (size_t)(nprog - entry_idx - 1) * sizeof(instr_t));

    for (int i = 0; i < nglobal_pins; i++) {
        instr_t ld; memset(&ld, 0, sizeof(ld));
        /* OP_LEA for an address pin ('= &SYM'): loads &SYM, not SYM's
           value, so later dereferences through this register always
           see SYM's live contents (see the "Global register pinning"
           comment above). OP_LOAD for an ordinary value pin, unchanged
           from before address pins existed. */
        ld.op = global_pins[i].is_addr ? OP_LEA : OP_LOAD;
        ld.src.kind = OPND_SYM;
        strncpy(ld.src.sym, global_pins[i].sym, MAX_SYMLEN - 1);
        ld.dst.kind = OPND_REG;
        ld.dst.reg_num = global_pins[i].reg_num;
        prog[entry_idx + 1 + i] = ld;
    }
    nprog += nglobal_pins;
    g_pin_load_start_idx = entry_idx + 1;
    g_pin_load_end_idx = entry_idx + nglobal_pins; /* inclusive */
}

int operand_hits_pin(const operand_t *o, global_pin_t **out_pin) {
    if (o->kind != OPND_REG || o->is_sp || o->is_float) return 0;
    global_pin_t *p = find_pin_by_reg(o->reg_num);
    if (!p) return 0;
    *out_pin = p;
    return 1;
}

void fail_pin_violation(const instr_t *ins, global_pin_t *p) {
    g_line_no = ins->src_line;
    g_filename = ins->src_file ? ins->src_file : g_filename;
    g_source_line = NULL; /* post-parse: no pp_lines[] text tracked
        against instr_t, and whatever g_source_line still holds from
        the last-parsed line would mismatch ins->src_line -- omit the
        snippet rather than show the wrong one */
    char msg[256];
    snprintf(msg, sizeof(msg),
        "register r%d is pinned to %s'%s' (see 'global r%d = %s%s;') and cannot be written to",
        p->reg_num, p->is_addr ? "the address of " : "", p->sym,
        p->reg_num, p->is_addr ? "&" : "", p->sym);
    fail(msg);
}

void check_addr_of_operand(const operand_t *o, const char *where) {
    if ((o->kind == OPND_SYM || o->kind == OPND_LOCAL) && o->is_addr_of) {
        char msg[384];
        snprintf(msg, sizeof(msg),
            "'&%s' (address-of) is not supported %s -- '&SYM'/'&local' is only accepted as a syscall(...)/libcall(...) argument, or as 'mv &%s > rX;' to load its address into a register",
            o->sym, where, o->sym);
        fail(msg);
    }
}

void check_addr_of_violations(void) {
    for (int idx = 0; idx < nprog; idx++) {
        const instr_t *ins = &prog[idx];
        g_line_no = ins->src_line;
        g_filename = ins->src_file ? ins->src_file : g_filename;
        g_source_line = NULL; /* see fail_pin_violation's comment */
        if (ins->op == OP_SYSCALL) {
            /* arg[0] = syscall number: never valid as '&SYM'. */
            if (ins->nargs > 0) check_addr_of_operand(&ins->args[0], "as a syscall number");
            /* arg[1..nargs-1]: '&SYM' is exactly what these are for. */
            continue;
        }
        if (ins->op == OP_LIBC_CALL) {
            /* Every libcall argument may be '&SYM'/'&local' -- unlike
               syscall(), there's no leading "number" slot to exempt, and
               a pointer-taking libc function (printf's format string,
               strlen's buffer, etc.) is the whole point of supporting
               '&' here at all. */
            continue;
        }
        check_addr_of_operand(&ins->src, "here");
        check_addr_of_operand(&ins->dst, "here");
        check_addr_of_operand(&ins->base_reg, "here");
        for (int a = 0; a < ins->nargs && a < 7; a++)
            check_addr_of_operand(&ins->args[a], "here");
    }
}

void check_argv_pin_collision(void) {
    if (!g_argv_seen) return;

    if (nglobal_pins > 0) {
        char msg[160];
        global_pin_t *hit = find_pin_by_reg(g_argv_argc_reg);
        if (hit) {
            snprintf(msg, sizeof(msg), "'%%argv': r%d (argc) is already pinned to '%s' via 'global' -- choose a different register", g_argv_argc_reg, hit->sym);
            fail(msg);
        }
        hit = find_pin_by_reg(g_argv_argv_reg);
        if (hit) {
            snprintf(msg, sizeof(msg), "'%%argv': r%d (argv) is already pinned to '%s' via 'global' -- choose a different register", g_argv_argv_reg, hit->sym);
            fail(msg);
        }
    }

    /* x86-64-only hazard: the heap seed that runs immediately after
       argv capture (see the OP_LABEL/is_entry case in emit_x86_64)
       uses 'rax' (r1) as its scratch, unlike AArch64/RISC-V whose
       entry-prologue scratch registers (x12/x13, t0/t1) sit outside
       the user-addressable r1-r12 range entirely. On x86-64, picking
       r1 for either %argv register would have its freshly-captured
       argc/argv value silently overwritten the instant the heap seed
       runs, if the program uses alloc() at all -- so it's rejected up
       front as a hard error rather than left as a target-specific trap
       that only x86-64 builds would ever hit. */
    if (g_uses_heap && g_target == TARGET_X86_64) {
        if (g_argv_argc_reg == 1) fail("'%argv': r1 (argc) collides with the heap-seed scratch register (rax) on x86-64 when alloc() is used -- choose a different register");
        if (g_argv_argv_reg == 1) fail("'%argv': r1 (argv) collides with the heap-seed scratch register (rax) on x86-64 when alloc() is used -- choose a different register");
    }
}

void check_init_scratch_collision(void) {
    if (g_init_scratch_seen) {
        global_pin_t *hit = find_pin_by_reg(g_init_scratch_reg);
        if (hit) {
            char msg[320];
            snprintf(msg, sizeof(msg),
                     "'%%iscratchr r%d': collides with 'global' pin of '%s' -- a 'local iK name = val;' initializer would silently clobber the pinned global the moment it ran; choose a different scratch register",
                     g_init_scratch_reg, hit->sym);
            fail(msg);
        }
    }
    if (g_finit_scratch_seen) {
        /* Float registers (f1-f7) and integer registers (r1-r12) are
           disjoint namespaces that happen to share reg_num values (see
           Bug 1's note_written_register fix) -- global_pins[] only ever
           holds integer pins, so a bare find_pin_by_reg(reg_num) match
           here would be a false positive against an unrelated integer
           pin that merely shares g_finit_scratch_reg's number. Since
           pins can never be float (do_global_directive rejects
           is_float), no float scratch register can ever actually
           collide; this loop existing (rather than being skipped
           entirely) just future-proofs against that restriction ever
           being lifted, at which point global_pins[] would need an
           is_float field to check against here too. */
    }
}

void check_global_pin_violations(void) {
    if (nglobal_pins == 0) return;

    for (int idx = 0; idx < nprog; idx++) {
        /* Skip the pin-load instructions themselves -- each pinned
           register's one and only legitimate write. */
        if (idx >= g_pin_load_start_idx && idx <= g_pin_load_end_idx) continue;

        const instr_t *ins = &prog[idx];
        global_pin_t *hit = NULL;

        switch (ins->op) {
            /* dst is a register write for all of these -- but OPND_LOCAL/
               OPND_SYM dst values (memory destinations, not registers)
               naturally fail operand_hits_pin's kind check above, so
               no extra branching is needed to exclude them. */
            case OP_MOV: case OP_LOAD: case OP_LEA:
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR:
            case OP_NOT: case OP_NEG:
            case OP_ROTL: case OP_ROTR:
            case OP_POPCOUNT: case OP_CLZ: case OP_CTZ:
            case OP_SAT_ADD: case OP_SAT_SUB:
            case OP_SEXT: case OP_ZEXT:
            case OP_ILOAD: case OP_LALOAD: case OP_HFIELD_LOAD: case OP_XLOAD:
            case OP_PTRADD: case OP_PTRSUB:
            case OP_POP: case OP_ALLOC:
                if (operand_hits_pin(&ins->dst, &hit)) fail_pin_violation(ins, hit);
                break;

            /* dst here names a memory location being written (a symbol
               or local), never a register -- operand_hits_pin already
               returns false for those kinds, but these ops are listed
               explicitly (rather than falling into `default`) so the
               "every opcode has a documented pin-check verdict" property
               holds even as new opcodes get added later. */
            case OP_STORE: case OP_ISTORE: case OP_LASTORE: case OP_HFIELD_STORE: case OP_XSTORE:
            case OP_CMP: case OP_PUSH: case OP_FENCE: case OP_RAWDATA:
            case OP_LABEL: case OP_FRAME_OPEN: case OP_FRAME_CLOSE:
            case OP_JMP: case OP_JE: case OP_JNE: case OP_JG: case OP_JL:
            case OP_JGE: case OP_JLE: case OP_JA: case OP_JB: case OP_JAE: case OP_JBE:
            case OP_CALL: case OP_RET: case OP_EXIT: case OP_STDOUT:
            case OP_READ: case OP_WRITE: case OP_HEAP_RESET: case OP_ASSERT:
            case OP_HALT:
                break;

            /* bcopyN rDST, rSRC, LEN: unlike OP_STORE above, dst here
               genuinely IS a register (parse-time requires it -- see
               'bcopyN' parsing), but it holds a *pointer value* being
               read and dereferenced for the copy's destination
               address, not a register being overwritten with a new
               value -- no different from OP_ISTORE's base_reg/idx_reg,
               which are also registers holding pointer/index values
               that get read, not written. A pinned global can
               correctly be read this way (pins only forbid being
               overwritten), so this is deliberately a no-op case, same
               reasoning as the block above, just needing its own entry
               since dst is a register instead of a memory-location
               operand kind operand_hits_pin already filters out. */
            case OP_BCOPY:
                break;

            case OP_SYSCALL:
                /* args[0..6] are read-only inputs (syscall number +
                   operands); a syscall's return value isn't captured
                   into a register by Chard's OP_SYSCALL form at all, so
                   there is no write here to check. */
                break;

            case OP_ATOMIC_ADD: case OP_ATOMIC_SUB: case OP_ATOMIC_AND:
            case OP_ATOMIC_OR: case OP_ATOMIC_XOR: case OP_ATOMIC_SWAP:
            case OP_ATOMIC_MAX: case OP_ATOMIC_MIN:
            case OP_ATOMIC_CAS:
                /* dst is the memory location (SYM/LOCAL), not a register;
                   result_reg is the actual register write. cas_expected/
                   cas_desired are read-only inputs. */
                if (operand_hits_pin(&ins->result_reg, &hit)) fail_pin_violation(ins, hit);
                break;

            case OP_I2S:
                /* i2s rSRC > rBUF, rLEN: src is a read-only input; dst
                   (rBUF) and len_reg (rLEN) are both register writes. */
                if (operand_hits_pin(&ins->dst, &hit)) fail_pin_violation(ins, hit);
                if (operand_hits_pin(&ins->len_reg, &hit)) fail_pin_violation(ins, hit);
                break;

            case OP_S2I:
                /* s2i rBUF, rLEN > rDST: dst (rBUF) and len_reg (rLEN)
                   are read-only inputs; result_reg (rDST) is the one
                   register write. */
                if (operand_hits_pin(&ins->result_reg, &hit)) fail_pin_violation(ins, hit);
                break;

            case OP_BCMP:
                /* bcmpN rDST, rPTR1, rPTR2, LEN: dst (rDST) is the one
                   register write; src (rPTR1), base_reg (rPTR2), and
                   len_reg (LEN, when a register) are read-only inputs. */
                if (operand_hits_pin(&ins->dst, &hit)) fail_pin_violation(ins, hit);
                break;

            /* Float-register writes (fDST in f1-f8) can never collide with
               a pinned register: pins are integer-only (parse_global_pin
               rejects a float register at parse time), and is_float
               operands are already excluded by operand_hits_pin. Listed
               here anyway, rather than falling into `default`, purely so
               this switch keeps its "every opcode has a documented
               verdict" property as new opcodes are added. */
            case OP_FMOV: case OP_FLOAD: case OP_FADD: case OP_FSUB:
            case OP_FMUL: case OP_FDIV: case OP_I2F:
            case OP_FSQRT: case OP_FABS: case OP_FNEG: case OP_FMIN: case OP_FMAX: case OP_FMA:
            case OP_VADD: case OP_VSUB: case OP_VMUL: case OP_VDIV:
            case OP_VMIN: case OP_VMAX: case OP_VSQRT: case OP_VABS: case OP_VNEG: case OP_VDUP:
            case OP_VFMA: case OP_VFMS: case OP_VFNMA:
            case OP_VLOAD:
                break;

            /* fstore SRC > SYM, vstore SRC > SYM, and fcmp all write no
               register at all (SYM is memory; fcmp only sets flags). */
            case OP_FSTORE: case OP_VSTORE: case OP_FCMP:
                break;

            /* f2i fSRC > rDST: the one float-family op with a genuine
               integer register write -- rDST can collide with a pin. */
            case OP_F2I:
                if (operand_hits_pin(&ins->dst, &hit)) fail_pin_violation(ins, hit);
                break;

            case OP_RAW:
                /* Opaque target-specific assembly text (see the 'raw'
                   parsing comment) -- Chard has no structured view into
                   what it writes, so it cannot be checked here. This is
                   the one documented gap in the enforcement: pinning
                   provides no guarantee across a 'raw' statement. */
                break;

            case OP_LIBC_INIT:
                /* No register write of its own -- see each backend's
                   OP_LIBC_INIT case for what it actually emits. */
                break;

            case OP_LIBC_CALL:
                /* args[0..nargs-1] are read-only inputs; dst (when
                   '> rX' was written) is the one register write,
                   exactly like OP_CALL's optional result capture. */
                if (ins->dst.kind == OPND_REG && operand_hits_pin(&ins->dst, &hit)) fail_pin_violation(ins, hit);
                break;
        }
    }
}

