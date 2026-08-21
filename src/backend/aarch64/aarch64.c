#include "../../chard.h"

const char *aarch64_local_base(FILE *out, const operand_t *o) {
    if (o->frames_up == 0) return "x29";
    fprintf(out, "    ldr %s, [x29]\n", SCRATCH_ARM);
    for (int i = 1; i < o->frames_up; i++)
        fprintf(out, "    ldr %s, [%s]\n", SCRATCH_ARM, SCRATCH_ARM);
    return SCRATCH_ARM;
}

const char *aarch64_safe_offset(FILE *out, const char *base, long offset,
                                        int sz, const char *scratch, long *out_offset) {
    int fits_unscaled = (offset >= -256 && offset <= 255);
    int fits_scaled = (offset >= 0 && offset <= (long)4095 * sz && (offset % sz) == 0);
    if (fits_unscaled || fits_scaled) {
        *out_offset = offset;
        return base;
    }
    /* Materialize base +/- |offset| into scratch, then access at
       offset 0. add/sub take a 12-bit unsigned immediate, optionally
       "lsl #12" shifted -- split the magnitude across both if it
       doesn't fit in the low 12 bits alone. */
    long mag = offset < 0 ? -offset : offset;
    const char *mn = offset < 0 ? "sub" : "add";
    long lo = mag & 0xFFF;
    long hi = mag >> 12;
    if (hi > 0xFFF) {
        /* Beyond even the shifted 24-bit range (+-16MB) -- not
           reachable with Chard's 1 MiB local-array limit, but fail
           loudly rather than silently emit a truncated address. */
        g_source_line = NULL;
        fail_fmt("internal error: local/field offset %ld exceeds AArch64 addressable range", offset);
    }
    if (hi > 0) fprintf(out, "    %s %s, %s, #%ld, lsl #12\n", mn, scratch, base, hi);
    if (lo > 0 || hi == 0) fprintf(out, "    %s %s, %s, #%ld\n", mn, scratch, hi > 0 ? scratch : base, lo);
    *out_offset = 0;
    return scratch;
}

void emit_aarch64_load_scratch(FILE *out, const operand_t *o) {
    int sz = operand_mem_size(o);
    if (o->kind == OPND_LOCAL) {
        /* A local's slot is x29 (fp)-relative, so it needs no adrp+add
           address computation at all -- straight to the load, unlike
           the pair a linker symbol requires. Offset/size were captured
           directly on the operand at parse time (see parse__operand),
           not re-looked-up here, since a local's decls[] entry no
           longer exists by the time codegen runs. Addressing via a
           dedicated frame pointer (established once per @label block
           at OP_FRAME_OPEN, restored at OP_FRAME_CLOSE) rather than sp
           directly is what keeps these offsets correct across nested
           blocks and any push/pop within a block that also has
           locals -- see the Locals section up top. A reference to a
           local declared in an enclosing block (frames_up > 0) chases
           the saved-fp chain first via aarch64_local_base. */
        const char *base = aarch64_local_base(out, o);
        long safe_off;
        /* Materializing into SCRATCH_ARM (x12) here would collide with
           the w12/x12 destination register this function always loads
           into, so use x13 as the address scratch when the offset
           needs to be materialized -- x13 isn't otherwise live at this
           point (it's only ever used transiently elsewhere, never
           across this call). */
        base = aarch64_safe_offset(out, base, -(long)o->local_offset, sz, "x13", &safe_off);
        if (sz == 1) fprintf(out, "    ldrb w12, [%s, #%ld]\n", base, safe_off);
        else if (sz == 2) fprintf(out, "    ldrh w12, [%s, #%ld]\n", base, safe_off);
        else if (sz == 4) fprintf(out, "    ldr w12, [%s, #%ld]\n", base, safe_off);
        else fprintf(out, "    ldr %s, [%s, #%ld]\n", SCRATCH_ARM, base, safe_off);
        return;
    }
    fprintf(out, "    adrp %s, %s\n", SCRATCH_ARM, o->sym);
    fprintf(out, "    add %s, %s, :lo12:%s\n", SCRATCH_ARM, SCRATCH_ARM, o->sym);
    if (sz == 1) fprintf(out, "    ldrb w12, [%s]\n", SCRATCH_ARM);
    else if (sz == 2) fprintf(out, "    ldrh w12, [%s]\n", SCRATCH_ARM);
    else if (sz == 4) fprintf(out, "    ldr w12, [%s]\n", SCRATCH_ARM);
    else fprintf(out, "    ldr %s, [%s]\n", SCRATCH_ARM, SCRATCH_ARM);
}

void aarch64_emit_local_addr(FILE *out, const char *dst, const char *base, int magnitude) {
    long lo = magnitude & 0xFFF;
    long hi = magnitude >> 12;
    if (hi > 0xFFF) {
        g_source_line = NULL;
        fail_fmt("internal error: local offset %d exceeds AArch64 addressable range", magnitude);
    }
    if (hi > 0) {
        fprintf(out, "    sub %s, %s, #%ld, lsl #12\n", dst, base, hi);
        if (lo > 0) fprintf(out, "    sub %s, %s, #%ld\n", dst, dst, lo);
    } else {
        fprintf(out, "    sub %s, %s, #%ld\n", dst, base, lo);
    }
}

const char *emit_aarch64_addr_into_scratch(FILE *out, const operand_t *o) {
    if (o->kind == OPND_LOCAL) {
        const char *base = aarch64_local_base(out, o);
        aarch64_emit_local_addr(out, SCRATCH_ARM, base, o->local_offset);
        return SCRATCH_ARM;
    }
    if (o->kind == OPND_ADDR) {
        /* Absolute address, e.g. '[0xB8000]' -- unlike a symbol
           (adrp+:lo12: against a linker-known label), a raw numeric
           address has to be materialized as an immediate. GAS's 'mov'
           pseudo-op already synthesizes the correct movz/movk sequence
           for any 64-bit constant (same trust-the-assembler approach
           OPND_IMM's OP_MOV case already relies on for large immediates
           via render_simple_operand), so a single 'mov' here is
           sufficient and portable across however large the address is. */
        fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, o->imm);
        return SCRATCH_ARM;
    }
    fprintf(out, "    adrp %s, %s\n", SCRATCH_ARM, o->sym);
    fprintf(out, "    add %s, %s, :lo12:%s\n", SCRATCH_ARM, SCRATCH_ARM, o->sym);
    return SCRATCH_ARM;
}

void arm_emit_int_val(FILE *out, long val, const int *is_label, char *const *val_labels, int v, int first) {
    fprintf(out, "%s", first ? "" : ", ");
    if (is_label && is_label[v]) fprintf(out, "%s", val_labels[v]);
    else fprintf(out, "%ld", val);
}

void emit__aarch64(FILE *out) {
    fprintf(out, "// Generated by Chard - target: AArch64\n");
    fprintf(out, "// GAS syntax\n");
    fprintf(out, g_mode == MODE_BARE
        ? "// mode: bare (raw/freestanding -- no ELF linkage scaffolding emitted)\n\n"
        : "// mode: elf (Linux)\n\n");

    fprintf(out, ".data\n");
    for (int i = 0; i < ndecls; i++) {
        if (decls[i].section == SEC_DATA) {
            if (decls[i].is_data_array) {
                const char *dsz = decls[i].size_bytes == 1 ? ".byte" :
                                   decls[i].size_bytes == 2 ? ".hword" :
                                   decls[i].size_bytes == 4 ? ".word" : ".xword";
                fprintf(out, "%s:\n    %s ", decls[i].name, dsz);
                if (decls[i].is_float) {
                    for (int v = 0; v < decls[i].array_len; v++) {
                        if (decls[i].size_bytes == 4)
                            fprintf(out, "%s0x%08x", v == 0 ? "" : ", ", float__bits(decls[i].data_fvals[v]));
                        else
                            fprintf(out, "%s0x%016llx", v == 0 ? "" : ", ", (unsigned long long)double__bits(decls[i].data_fvals[v]));
                    }
                } else {
                    for (int v = 0; v < decls[i].array_len; v++)
                        arm_emit_int_val(out, decls[i].data_vals[v], decls[i].data_val_is_label, decls[i].data_val_labels, v, v == 0);
                }
                fprintf(out, "\n");
            } else if (decls[i].is_ascii) {
                fprintf(out, "%s:\n    .byte ", decls[i].name);
                for (int b = 0; b < decls[i].str_len; b++) {
                    fprintf(out, "%s%d", b == 0 ? "" : ", ", (unsigned char)decls[i].str_val[b]);
                }
                if (decls[i].str_len == 0) fprintf(out, "0");
                fprintf(out, "\n%s_len:\n    .xword %d\n", decls[i].name, decls[i].str_len);
            } else if (decls[i].is_float) {
                if (decls[i].size_bytes == 4)
                    fprintf(out, "%s:\n    .word 0x%08x\n", decls[i].name, float__bits(decls[i].init_fvalue));
                else
                    fprintf(out, "%s:\n    .xword 0x%016llx\n", decls[i].name, (unsigned long long)double__bits(decls[i].init_fvalue));
            } else {
                const char *dsz = decls[i].size_bytes == 1 ? ".byte" :
                                   decls[i].size_bytes == 2 ? ".hword" :
                                   decls[i].size_bytes == 4 ? ".word" : ".xword";
                fprintf(out, "%s:\n    %s %ld\n", decls[i].name, dsz, decls[i].init_value);
            }
        }
    }
    /* 'rodata' -- see the x86-64 emitter's matching '.rodata' loop
       comment for the full rationale; same per-decl-kind emission as
       '.data' above, just its own section. */
    fprintf(out, "\n.rodata\n");
    for (int i = 0; i < ndecls; i++) {
        if (decls[i].section == SEC_RODATA) {
            if (decls[i].is_data_array) {
                const char *dsz = decls[i].size_bytes == 1 ? ".byte" :
                                   decls[i].size_bytes == 2 ? ".hword" :
                                   decls[i].size_bytes == 4 ? ".word" : ".xword";
                fprintf(out, "%s:\n    %s ", decls[i].name, dsz);
                if (decls[i].is_float) {
                    for (int v = 0; v < decls[i].array_len; v++) {
                        if (decls[i].size_bytes == 4)
                            fprintf(out, "%s0x%08x", v == 0 ? "" : ", ", float__bits(decls[i].data_fvals[v]));
                        else
                            fprintf(out, "%s0x%016llx", v == 0 ? "" : ", ", (unsigned long long)double__bits(decls[i].data_fvals[v]));
                    }
                } else {
                    for (int v = 0; v < decls[i].array_len; v++)
                        arm_emit_int_val(out, decls[i].data_vals[v], decls[i].data_val_is_label, decls[i].data_val_labels, v, v == 0);
                }
                fprintf(out, "\n");
            } else if (decls[i].is_ascii) {
                fprintf(out, "%s:\n    .byte ", decls[i].name);
                for (int b = 0; b < decls[i].str_len; b++) {
                    fprintf(out, "%s%d", b == 0 ? "" : ", ", (unsigned char)decls[i].str_val[b]);
                }
                if (decls[i].str_len == 0) fprintf(out, "0");
                fprintf(out, "\n%s_len:\n    .xword %d\n", decls[i].name, decls[i].str_len);
            } else if (decls[i].is_float) {
                if (decls[i].size_bytes == 4)
                    fprintf(out, "%s:\n    .word 0x%08x\n", decls[i].name, float__bits(decls[i].init_fvalue));
                else
                    fprintf(out, "%s:\n    .xword 0x%016llx\n", decls[i].name, (unsigned long long)double__bits(decls[i].init_fvalue));
            } else {
                const char *dsz = decls[i].size_bytes == 1 ? ".byte" :
                                   decls[i].size_bytes == 2 ? ".hword" :
                                   decls[i].size_bytes == 4 ? ".word" : ".xword";
                fprintf(out, "%s:\n    %s %ld\n", decls[i].name, dsz, decls[i].init_value);
            }
        }
    }
    fprintf(out, "\n.bss\n");
    for (int i = 0; i < ndecls; i++) {
        if (decls[i].section == SEC_BSS) {
            if (decls[i].is_ascii) {
                /* See the x86-64 emitter's matching branch: N
                   uninitialized bytes plus an uninitialized 8-byte
                   'name_len', both in .bss. */
                fprintf(out, "%s:\n    .skip %d\n", decls[i].name, decls[i].array_len);
                fprintf(out, "%s_len:\n    .skip 8\n", decls[i].name);
                continue;
            }
            /* '.skip' takes total bytes, unlike NASM's resN (element
               count) -- so a 'bss iK name[N];' array reserves
               size_bytes*array_len bytes here, vs. just size_bytes for
               an ordinary scalar bss decl (array_len 0; see decl_t's
               own comment on the field). */
            int count = decls[i].array_len > 0 ? decls[i].array_len : 1;
            fprintf(out, "%s:\n    .skip %ld\n", decls[i].name, (long)decls[i].size_bytes * count);
        }
    }
    if (g_uses_heap) {
        fprintf(out, "%s:\n    .skip %ld\n", HEAP_SYM, g_heap_size_bytes);
        fprintf(out, "%s:\n    .skip 8\n", HEAP_PTR_SYM);
    }

    fprintf(out, "\n.text\n");
    if (g_mode == MODE_BARE) {
        /* See the matching x86-64 comment: no '.global'/'.extern' in
           BARE/raw output. */
    } else {
        fprintf(out, ".global %s\n", entry_label);
        for (int i = 0; i < nexterns; i++) {
            if (externs[i].lib[0])
                fprintf(out, ".extern %s // from -l%s -- see build note printed after compilation\n", externs[i].name, externs[i].lib);
            else
                fprintf(out, ".extern %s\n", externs[i].name);
        }
        if (g_libc_linked) fprintf(out, ".extern exit\n");
    }
    fprintf(out, "\n");

    for (int i = 0; i < nprog; i++) {
        instr_t *ins = &prog[i];
        char sb[64];
        /* CFI (GAS .cfi_* directives): only under ELF mode (BARE has no
           unwinder to read it -- see check_bare_mode_requirements) and
           only inside a function-root body (bracketed by is_func_start
           on this function's OP_LABEL and its func_frame_close_idx --
           see instr_t's comments). cfi_active is true from that OP_LABEL
           through its OP_FRAME_CLOSE. cfi_push_bytes tracks how many
           bytes OP_PUSH has moved sp by since OP_FRAME_OPEN established
           x29 as the CFA base, so each push's .cfi_offset states its own
           CFA-relative slot -- mirrors OP_PUSH/OP_POP's own
           stack_slot_size() bookkeeping, kept separate from frame_bytes
           since that already means declared-local space and is
           back-patched independently. */
        static int cfi_active = 0;
        static int cfi_push_bytes = 0;

        switch (ins->op) {
        case OP_LABEL:
            if (g_mode == MODE_ELF && ins->is_func_start) {
                fprintf(out, ".cfi_startproc\n");
                cfi_active = 1;
                cfi_push_bytes = 0;
            }
            fprintf(out, "%s:\n", ins->dst.sym);

            if (ins->is_entry && g_argv_seen) {
                /* '%argv rN, rM;' capture -- see the matching x86-64
                   OP_LABEL case for the full rationale; same ordering
                   requirement (before OP_FRAME_OPEN moves sp, before
                   anything clobbers x0/x1). */
                const char *argc_r = target_defs[TARGET_AARCH64].regs[g_argv_argc_reg];
                const char *argv_r = target_defs[TARGET_AARCH64].regs[g_argv_argv_reg];
                if (g_libc_linked) {
                    /* main(int argc, char **argv): argc in w0 (32-bit
                       view of x0), argv in x1. 'mov w_, w0' on AArch64
                       always zero-extends into the full 64-bit register
                       (unlike x86-64 where this is also true but for a
                       different ISA-level reason), so a single 32-bit
                       move already leaves argc_r's upper bits clean --
                       no separate zeroing step needed. argc_r is always
                       one of target_defs[TARGET_AARCH64].regs[1..12]
                       ("x0".."x11"), so swapping the leading 'x' for
                       'w' gives its 32-bit alias directly. */
                    char argc_w[8];
                    snprintf(argc_w, sizeof(argc_w), "w%s", argc_r + 1);
                    fprintf(out, "    mov %s, w0\n", argc_w);
                    fprintf(out, "    mov %s, x1\n", argv_r);
                } else {
                    /* Freestanding _start: [sp] = argc, [sp+8] = argv
                       (i.e. sp+8 is argv itself), same layout as
                       x86-64's incoming rsp. Must be read before
                       OP_FRAME_OPEN adjusts sp. */
                    fprintf(out, "    ldr %s, [sp]\n", argc_r);
                    fprintf(out, "    add %s, sp, #8\n", argv_r);
                }
            }
            if (ins->is_entry && g_uses_heap) {
                /* Seed __heap_ptr = &__heap once, right as execution
                   begins. */
                fprintf(out, "    adrp %s, %s\n", SCRATCH_ARM, HEAP_SYM);
                fprintf(out, "    add %s, %s, :lo12:%s\n", SCRATCH_ARM, SCRATCH_ARM, HEAP_SYM);
                fprintf(out, "    adrp x13, %s\n", HEAP_PTR_SYM);
                fprintf(out, "    add x13, x13, :lo12:%s\n", HEAP_PTR_SYM);
                fprintf(out, "    str %s, [x13]\n", SCRATCH_ARM);
            }
            break;

        case OP_FRAME_OPEN: {
            /* Establishes this block's frame: save the caller's x29
               (fp) below the new sp, point x29 at that saved slot,
               then reserve frame_bytes for this block's locals (if
               any). AAPCS64 requires sp to stay 16-byte aligned at all
               times, so both the fp-save slot and the local
               reservation are rounded up to 16 -- 'str x29, [sp,
               #-16]!' pre-decrements sp by a full 16-byte-aligned
               chunk to save a single 8-byte register, which wastes 8
               bytes but keeps every subsequent sp value aligned
               without a separate accounting path for "the 8 bytes
               spent on fp" vs "the N bytes spent on locals". The
               save/establish step always runs, even for a block with
               zero locals of its own -- see the x86-64 OP_FRAME_OPEN
               case for why unconditional nesting support matters here. */
            /* CFI only describes the function-ROOT's own frame open, not
               every nested if/while/local block's -- cfi_is_root_open is
               true for exactly one OP_FRAME_OPEN per function (the one
               immediately following that function's OP_LABEL, since
               open_local_frame's call site pushes the label then opens
               its root frame with nothing in between -- see the '@name:'
               parsing site). A nested block's own OP_FRAME_OPEN still
               moves sp for its locals, but the unwinder doesn't need a
               second CFA description for it: DWARF's CFA is defined
               relative to a fixed register (x29) once, at
               .cfi_def_cfa_register below, and every register write
               after that point (nested sp adjustments included) doesn't
               move where x29 itself points, so the single root
               description already covers the whole function. */
            int cfi_is_root_open = (g_mode == MODE_ELF && cfi_active && i > 0 && prog[i - 1].op == OP_LABEL && prog[i - 1].is_func_start);
            fprintf(out, "    str x29, [sp, #-16]!\n");
            if (cfi_is_root_open) {
                fprintf(out, "    .cfi_def_cfa_offset 16\n");
                fprintf(out, "    .cfi_offset x29, -16\n");
            }
            fprintf(out, "    mov x29, sp\n");
            if (cfi_is_root_open) fprintf(out, "    .cfi_def_cfa_register x29\n");
            int rounded = (ins->frame_bytes + 15) & ~15;
            if (rounded > 0) fprintf(out, "    sub sp, sp, #%d\n", rounded);
            break;
        }

        case OP_FRAME_CLOSE: {
            /* 'mov sp, x29' undoes the local reservation in one step
               (mirroring x86-64's 'mov rsp, rbp'), then 'ldr x29, [sp],
               #16' restores the caller's x29 from the slot
               OP_FRAME_OPEN saved it in and pops the same 16-byte-
               aligned chunk that was reserved for it. */
            fprintf(out, "    mov sp, x29\n");
            fprintf(out, "    ldr x29, [sp], #16\n");
            /* Matching .cfi_endproc for this function's .cfi_startproc:
               fires on the specific OP_FRAME_CLOSE this function's
               OP_LABEL was back-patched with (func_frame_close_idx --
               see close_local_frame), not on a nested block's frame
               close, since a function can have several OP_FRAME_CLOSEs
               in the body (one per ret; -- see
               relocate_frame_close_before_rets) but only the LAST one
               reached at true end-of-body should end the procedure
               record; in practice func_frame_close_idx always names
               that one. */
            if (cfi_active) {
                int j;
                for (j = i - 1; j >= 0; j--) if (prog[j].op == OP_LABEL && prog[j].is_func_start) break;
                if (j >= 0 && prog[j].func_frame_close_idx == i) {
                    fprintf(out, "    .cfi_endproc\n");
                    cfi_active = 0;
                }
            }
            break;
        }


        case OP_MOV:
            if (ins->src.kind == OPND_IMM) {
                render_simple_operand(TARGET_AARCH64, &ins->src, sb, sizeof(sb));
                fprintf(out, "    mov %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst), sb);
            } else if (is_mem_operand(ins->src.kind)) {
                /* mov SYM > rX; / mov local > rX; -- see the matching
                   x86-64 OP_MOV comment for the bug this fixes (a
                   memory-operand source used to fall through to the
                   plain-register branch below and read garbage out of
                   reg__name()). Same scratch-load pattern used
                   elsewhere on this backend for a memory source. */
                emit_aarch64_load_scratch(out, &ins->src);
                fprintf(out, "    mov %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst), SCRATCH_ARM);
            } else {
                fprintf(out, "    mov %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst),
                        reg__name(TARGET_AARCH64, &ins->src));
            }
            break;

        case OP_LOAD: {
            int sz = operand_mem_size(&ins->src);
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            char wreg[16];
            snprintf(wreg, sizeof(wreg), "w%s", dstreg + 1); /* x0 -> w0, for 32-bit-and-under loads */
            /* Mnemonic per width, selected once for reuse across all
               three source-kind branches below (local/addr/symbol) --
               they're otherwise byte-for-byte identical apart from the
               address computation that precedes each. AArch64 spells
               sign-extending loads with an 's' inserted before the
               size letter (ldrb->ldrsb, ldrh->ldrsh), and a 32-bit
               sign-extending load into a 64-bit dest is 'ldrsw' (using
               the full 'dstreg', not 'wreg' -- unlike every other case
               here, a 32->64 sign-extend needs the wide destination
               name since it's explicitly producing 64 sign-extended
               bits, not just writing the low 32 and letting the
               architecture zero the rest the way plain 'ldr wN'
               does). 8-byte loads never extend either way, signed or
               not -- same 'ldr dstreg' either way. */
            const char *mn1 = ins->load_signed ? "ldrsb" : "ldrb";
            const char *mn2 = ins->load_signed ? "ldrsh" : "ldrh";
            const char *mn4 = ins->load_signed ? "ldrsw" : "ldr";
            const char *reg4 = ins->load_signed ? dstreg : wreg; /* ldrsw writes the full 64-bit reg; plain ldr (32-bit) writes wreg */
            if (ins->src.kind == OPND_LOCAL) {
                const char *base = aarch64_local_base(out, &ins->src);
                long safe_off;
                /* dstreg is being written by this very load, so it's
                   safe to reuse as address scratch if materialization
                   is needed -- the address computation completes and
                   is consumed before the load overwrites it. */
                base = aarch64_safe_offset(out, base, -(long)ins->src.local_offset, sz, dstreg, &safe_off);
                if (sz == 1) fprintf(out, "    %s %s, [%s, #%ld]\n", mn1, wreg, base, safe_off);
                else if (sz == 2) fprintf(out, "    %s %s, [%s, #%ld]\n", mn2, wreg, base, safe_off);
                else if (sz == 4) fprintf(out, "    %s %s, [%s, #%ld]\n", mn4, reg4, base, safe_off);
                else fprintf(out, "    ldr %s, [%s, #%ld]\n", dstreg, base, safe_off);
                break;
            }
            if (ins->src.kind == OPND_ADDR) {
                /* Absolute address -- materialize into SCRATCH_ARM via
                   'mov', then load through it at offset 0, mirroring
                   the symbol path below but skipping adrp/:lo12: (which
                   only apply to linker-known symbols, not a bare
                   numeric constant). */
                emit_aarch64_addr_into_scratch(out, &ins->src);
                if (sz == 1) fprintf(out, "    %s %s, [%s]\n", mn1, wreg, SCRATCH_ARM);
                else if (sz == 2) fprintf(out, "    %s %s, [%s]\n", mn2, wreg, SCRATCH_ARM);
                else if (sz == 4) fprintf(out, "    %s %s, [%s]\n", mn4, reg4, SCRATCH_ARM);
                else fprintf(out, "    ldr %s, [%s]\n", dstreg, SCRATCH_ARM);
                break;
            }
            fprintf(out, "    adrp %s, %s\n", SCRATCH_ARM, ins->src.sym);
            fprintf(out, "    add %s, %s, :lo12:%s\n", SCRATCH_ARM, SCRATCH_ARM, ins->src.sym);
            if (sz == 1) fprintf(out, "    %s %s, [%s]\n", mn1, wreg, SCRATCH_ARM);
            else if (sz == 2) fprintf(out, "    %s %s, [%s]\n", mn2, wreg, SCRATCH_ARM);
            else if (sz == 4) fprintf(out, "    %s %s, [%s]\n", mn4, reg4, SCRATCH_ARM);
            else fprintf(out, "    ldr %s, [%s]\n", dstreg, SCRATCH_ARM);
            break;
        }

        case OP_STORE: {
            int sz = operand_mem_size(&ins->dst);
            const char *srcreg = reg__name(TARGET_AARCH64, &ins->src);
            char wreg[16];
            snprintf(wreg, sizeof(wreg), "w%s", srcreg + 1);
            if (ins->dst.kind == OPND_LOCAL) {
                const char *base = aarch64_local_base(out, &ins->dst);
                long safe_off;
                /* Unlike OP_LOAD, srcreg holds the value being stored
                   and can't be clobbered as address scratch -- use
                   SCRATCH_ARM (x12). aarch64_local_base may itself
                   already have used SCRATCH_ARM for the fp chase (when
                   frames_up > 0), which is fine: base already points
                   at x12's chased value, and materializing the offset
                   in-place into x12 (base==scratch) is safe since the
                   chase result is only needed as an input to that same
                   add/sub. */
                base = aarch64_safe_offset(out, base, -(long)ins->dst.local_offset, sz, SCRATCH_ARM, &safe_off);
                if (sz == 1) fprintf(out, "    strb %s, [%s, #%ld]\n", wreg, base, safe_off);
                else if (sz == 2) fprintf(out, "    strh %s, [%s, #%ld]\n", wreg, base, safe_off);
                else if (sz == 4) fprintf(out, "    str %s, [%s, #%ld]\n", wreg, base, safe_off);
                else fprintf(out, "    str %s, [%s, #%ld]\n", srcreg, base, safe_off);
                break;
            }
            if (ins->dst.kind == OPND_ADDR) {
                emit_aarch64_addr_into_scratch(out, &ins->dst);
                if (sz == 1) fprintf(out, "    strb %s, [%s]\n", wreg, SCRATCH_ARM);
                else if (sz == 2) fprintf(out, "    strh %s, [%s]\n", wreg, SCRATCH_ARM);
                else if (sz == 4) fprintf(out, "    str %s, [%s]\n", wreg, SCRATCH_ARM);
                else fprintf(out, "    str %s, [%s]\n", srcreg, SCRATCH_ARM);
                break;
            }
            fprintf(out, "    adrp %s, %s\n", SCRATCH_ARM, ins->dst.sym);
            fprintf(out, "    add %s, %s, :lo12:%s\n", SCRATCH_ARM, SCRATCH_ARM, ins->dst.sym);
            if (sz == 1) fprintf(out, "    strb %s, [%s]\n", wreg, SCRATCH_ARM);
            else if (sz == 2) fprintf(out, "    strh %s, [%s]\n", wreg, SCRATCH_ARM);
            else if (sz == 4) fprintf(out, "    str %s, [%s]\n", wreg, SCRATCH_ARM);
            else fprintf(out, "    str %s, [%s]\n", srcreg, SCRATCH_ARM);
            break;
        }

        case OP_LEA: {
            /* Address of the symbol itself, computed directly into the
               destination register. For a local this is just an
               fp-relative 'sub' (no adrp needed -- the address is
               already position-relative to the block's own frame
               pointer, not link-time fixed), chasing saved-fp links
               first via aarch64_local_base if the local was declared in
               an enclosing block. */
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            if (ins->src.kind == OPND_LOCAL) {
                const char *base = aarch64_local_base(out, &ins->src);
                /* dstreg is free to use as the running accumulator
                   since nothing else needs it yet. */
                aarch64_emit_local_addr(out, dstreg, base, ins->src.local_offset);
                break;
            }
            fprintf(out, "    adrp %s, %s\n", dstreg, ins->src.sym);
            fprintf(out, "    add %s, %s, :lo12:%s\n", dstreg, dstreg, ins->src.sym);
            break;
        }

        case OP_ADD: case OP_SUB: case OP_AND: case OP_OR:
        case OP_XOR: case OP_SHL: case OP_SHR: case OP_CMP: {
            const char *mn = ins->op == OP_ADD ? "add" : ins->op == OP_SUB ? "sub" :
                              ins->op == OP_AND ? "and" : ins->op == OP_OR ? "orr" :
                              ins->op == OP_XOR ? "eor" : ins->op == OP_SHL ? "lsl" :
                              ins->op == OP_SHR ? "lsr" : "cmp";
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                if (ins->op == OP_CMP)
                    fprintf(out, "    cmp %s, %s\n", dstreg, SCRATCH_ARM);
                else
                    fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, SCRATCH_ARM);
            } else {
                render_simple_operand(TARGET_AARCH64, &ins->src, sb, sizeof(sb));
                if (ins->op == OP_CMP)
                    fprintf(out, "    cmp %s, %s\n", dstreg, sb);
                else
                    fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, sb);
            }
            break;
        }

        case OP_MUL: {
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                fprintf(out, "    mul %s, %s, %s\n", dstreg, dstreg, SCRATCH_ARM);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
                fprintf(out, "    mul %s, %s, %s\n", dstreg, dstreg, SCRATCH_ARM);
            } else {
                fprintf(out, "    mul %s, %s, %s\n", dstreg, dstreg, reg__name(TARGET_AARCH64, &ins->src));
            }
            break;
        }

        case OP_NOT: case OP_NEG: {
            /* AArch64 has true unary reg,reg instructions for both
               ('mvn' bitwise-NOT, 'neg' two's-complement negate) --
               no move-into-dst-first dance needed like x86-64's
               not/neg, which are strictly single-operand in-place. An
               immediate/memory src is materialized into scratch first
               (same pattern as OP_MUL above), since mvn/neg's source
               operand is a register (or shifted register), not an
               immediate. */
            const char *mn = ins->op == OP_NOT ? "mvn" : "neg";
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                fprintf(out, "    %s %s, %s\n", mn, dstreg, SCRATCH_ARM);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
                fprintf(out, "    %s %s, %s\n", mn, dstreg, SCRATCH_ARM);
            } else {
                fprintf(out, "    %s %s, %s\n", mn, dstreg, reg__name(TARGET_AARCH64, &ins->src));
            }
            break;
        }

        case OP_ROTL: case OP_ROTR: {
            /* AArch64 has no native rotate-left, only 'ror' (rotate
               right, register or #imm count) -- rotl by N is ror by
               (64-N), which the ISA itself expects mod-64 (ror's
               register-count form masks to the low 6 bits in hardware,
               same as x86-64's cl-staged shift count), so 'neg' on the
               count register naturally produces the right (64-N) mod 64
               value without a separate mod-64 step: neg computes
               0-N, and a subsequent 6-bit-masked ror of 0-N is
               bit-identical to a 6-bit-masked ror of 64-N. An immediate
               count doesn't have this luxury (there's no 'ror #-N'
               syntax), so it's negated directly in C via '64 - (N % 64)'
               before being materialized as a literal. */
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            if (ins->op == OP_ROTR) {
                if (ins->src.kind == OPND_IMM) {
                    fprintf(out, "    ror %s, %s, #%ld\n", dstreg, dstreg, ((ins->src.imm % 64) + 64) % 64);
                } else if (is_mem_operand(ins->src.kind)) {
                    emit_aarch64_load_scratch(out, &ins->src);
                    fprintf(out, "    ror %s, %s, %s\n", dstreg, dstreg, SCRATCH_ARM);
                } else {
                    fprintf(out, "    ror %s, %s, %s\n", dstreg, dstreg, reg__name(TARGET_AARCH64, &ins->src));
                }
            } else {
                if (ins->src.kind == OPND_IMM) {
                    long negcount = (64 - ((ins->src.imm % 64) + 64) % 64) % 64;
                    fprintf(out, "    ror %s, %s, #%ld\n", dstreg, dstreg, negcount);
                } else if (is_mem_operand(ins->src.kind)) {
                    emit_aarch64_load_scratch(out, &ins->src);
                    fprintf(out, "    neg %s, %s\n", SCRATCH_ARM, SCRATCH_ARM);
                    fprintf(out, "    ror %s, %s, %s\n", dstreg, dstreg, SCRATCH_ARM);
                } else {
                    fprintf(out, "    neg x13, %s\n", reg__name(TARGET_AARCH64, &ins->src));
                    fprintf(out, "    ror %s, %s, x13\n", dstreg, dstreg);
                }
            }
            break;
        }

        case OP_POPCOUNT: {
            /* No general-purpose-register popcount on AArch64 -- cnt
               operates per-byte on a SIMD/FP vector register, so the
               64-bit value is moved into a scalar FP register (fmov,
               which reinterprets the bit pattern rather than
               converting a value, exactly like x86-64's movq between
               a GPR and an xmm register), counted per-byte with cnt
               into an 8x8-bit vector, then addv horizontally sums those
               8 byte-counts back into a single scalar. d31 is used as
               scratch here (a float/SIMD register, not one of Chard's
               f1-f8 -- those live in d0-d6 per the fscratch/register
               table above, so d31 doesn't collide with anything a Chard
               program can itself name). */
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                fprintf(out, "    fmov d31, %s\n", SCRATCH_ARM);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
                fprintf(out, "    fmov d31, %s\n", SCRATCH_ARM);
            } else {
                fprintf(out, "    fmov d31, %s\n", reg__name(TARGET_AARCH64, &ins->src));
            }
            fprintf(out, "    cnt v31.8b, v31.8b\n");
            fprintf(out, "    addv b31, v31.8b\n");
            fprintf(out, "    fmov %s, d31\n", dstreg);
            fprintf(out, "    and %s, %s, #0xff\n", dstreg, dstreg);
            break;
        }

        case OP_CLZ: case OP_CTZ: {
            /* clz is native. ctz has no native AArch64 instruction, so
               it's synthesized as rbit (bit-reverse the whole 64-bit
               value) followed by clz of the reversed value -- trailing
               zeros of X are exactly leading zeros of X's bit-reversal,
               the standard idiom the AArch64 ISA itself documents for
               this gap. */
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            const char *srcreg;
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                srcreg = SCRATCH_ARM;
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
                srcreg = SCRATCH_ARM;
            } else {
                srcreg = reg__name(TARGET_AARCH64, &ins->src);
            }
            if (ins->op == OP_CLZ) {
                fprintf(out, "    clz %s, %s\n", dstreg, srcreg);
            } else {
                fprintf(out, "    rbit x13, %s\n", srcreg);
                fprintf(out, "    clz %s, x13\n", dstreg);
            }
            break;
        }

        case OP_SEXT: case OP_ZEXT: {
            /* sxtb/sxth/sxtw and uxtb/uxth are native AArch64
               instructions, register-to-register, no memory addressing
               mode to speak of -- an immediate source is first
               materialized into SCRATCH_ARM (x12) exactly like the
               CLZ/CTZ block above, then extended from there. There's
               no dedicated uxtw: a plain 32-bit 'mov w_, w_' already
               zero-extends into the full 64-bit register as an
               architectural guarantee on AArch64 (writing any Wn
               register always zeroes the upper 32 bits of the paired
               Xn register), the same free-zero-extension property
               x86-64's 32-bit 'mov' has and OP_ZEXT's x86-64 case above
               already relies on -- so zext32 reuses that instead of
               reaching for a named extend instruction that doesn't
               exist. sxtb/sxth/sxtw's destination is always a full Xn
               register regardless of source width, so dstreg is used
               directly rather than needing a width-narrowed view the
               way x86-64's sub-register table did. */
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            const char *srcreg;
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                srcreg = SCRATCH_ARM;
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
                srcreg = SCRATCH_ARM;
            } else {
                srcreg = reg__name(TARGET_AARCH64, &ins->src);
            }
            if (ins->op == OP_SEXT) {
                const char *mn = ins->elem_size == 1 ? "sxtb" : ins->elem_size == 2 ? "sxth" : "sxtw";
                fprintf(out, "    %s %s, %s\n", mn, dstreg, srcreg);
            } else if (ins->elem_size == 4) {
                /* Zero-extending 32-bit mov: srcreg's W-view written
                   into dstreg's W-view, upper 32 bits of dstreg zeroed
                   for free -- see comment above. srcreg/dstreg here are
                   already X-register names (e.g. "x3"); swapping the
                   leading 'x' for 'w' gives the paired 32-bit name,
                   same naming convention AArch64 itself uses. */
                fprintf(out, "    mov w%s, w%s\n", dstreg + 1, srcreg + 1);
            } else {
                const char *mn = ins->elem_size == 1 ? "uxtb" : "uxth";
                fprintf(out, "    %s %s, %s\n", mn, dstreg, srcreg);
            }
            break;
        }

        case OP_SAT_ADD: case OP_SAT_SUB: {
            /* adds/subs set the V (overflow) flag exactly like x86-64's
               OF -- csel then picks between the raw result and a
               saturated sentinel based on V (vs), and a second csel
               (keyed off the raw result's own sign, N) picks which
               sentinel (MAX on overflow-to-negative, MIN on overflow-
               to-positive), same reasoning as the x86-64 block's
               comment above. csel is branch-free and reads condition
               flags directly, so -- unlike x86-64's cmov, which needed
               push/pop to free up a temp -- this needs no register
               shuffling at all: x13/SCRATCH_ARM are both free to use
               as pure scratch throughout. */
            const char *mn = ins->op == OP_SAT_ADD ? "adds" : "subs";
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, SCRATCH_ARM);
            } else if (ins->src.kind == OPND_IMM && ins->src.imm >= 0 && ins->src.imm <= 4095) {
                fprintf(out, "    %s %s, %s, #%ld\n", mn, dstreg, dstreg, ins->src.imm);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, SCRATCH_ARM);
            } else {
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, reg__name(TARGET_AARCH64, &ins->src));
            }
            fprintf(out, "    movz x13, #0x0\n");
            fprintf(out, "    movk x13, #0x0, lsl #16\n");
            fprintf(out, "    movk x13, #0x0, lsl #32\n");
            fprintf(out, "    movk x13, #0x8000, lsl #48\n");
            fprintf(out, "    movz %s, #0xffff\n", SCRATCH_ARM);
            fprintf(out, "    movk %s, #0xffff, lsl #16\n", SCRATCH_ARM);
            fprintf(out, "    movk %s, #0xffff, lsl #32\n", SCRATCH_ARM);
            fprintf(out, "    movk %s, #0x7fff, lsl #48\n", SCRATCH_ARM);
            fprintf(out, "    csel x13, %s, x13, mi\n", SCRATCH_ARM);
            fprintf(out, "    csel %s, x13, %s, vs\n", dstreg, dstreg);
            break;
        }

        case OP_DIV: case OP_MOD: {
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            const char *SCRATCH_ARM2 = "x13"; /* holds the divisor for mod's msub step */
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
            } else {
                fprintf(out, "    mov %s, %s\n", SCRATCH_ARM, reg__name(TARGET_AARCH64, &ins->src));
            }
            if (ins->op == OP_DIV) {
                fprintf(out, "    sdiv %s, %s, %s\n", dstreg, dstreg, SCRATCH_ARM);
            } else {
                /* AArch64 has no remainder instruction: remainder = dst -
                   (quotient * divisor), computed with sdiv + msub so
                   nothing here silently loses the remainder either. */
                fprintf(out, "    mov %s, %s\n", SCRATCH_ARM2, SCRATCH_ARM);
                fprintf(out, "    sdiv %s, %s, %s\n", SCRATCH_ARM, dstreg, SCRATCH_ARM2);
                fprintf(out, "    msub %s, %s, %s, %s\n", dstreg, SCRATCH_ARM, SCRATCH_ARM2, dstreg);
            }
            break;
        }

        case OP_JMP: fprintf(out, ins->dst.kind == OPND_REG ? "    br %s\n" : "    b %s\n", ins->dst.kind == OPND_REG ? reg__name(TARGET_AARCH64, &ins->dst) : ins->dst.sym); break;
        case OP_JE:  fprintf(out, "    b.eq %s\n", ins->dst.sym); break;
        case OP_JNE: fprintf(out, "    b.ne %s\n", ins->dst.sym); break;
        case OP_JG:  fprintf(out, "    b.gt %s\n", ins->dst.sym); break;
        case OP_JL:  fprintf(out, "    b.lt %s\n", ins->dst.sym); break;
        case OP_JGE: fprintf(out, "    b.ge %s\n", ins->dst.sym); break;
        case OP_JLE: fprintf(out, "    b.le %s\n", ins->dst.sym); break;
        case OP_JA:  fprintf(out, "    b.hi %s\n", ins->dst.sym); break;
        case OP_JB:  fprintf(out, "    b.lo %s\n", ins->dst.sym); break;
        case OP_JAE: fprintf(out, "    b.hs %s\n", ins->dst.sym); break;
        case OP_JBE: fprintf(out, "    b.ls %s\n", ins->dst.sym); break;

        case OP_ASSERT: {
            /* Mirrors the x86-64 OP_ASSERT case: assert_jmp_op (the
               inverted condition from invert_cond_op) fires when the
               asserted condition is false, so jump on it straight to
               the trap label; otherwise jump past to ok_label. 'brk #0'
               is AArch64's breakpoint/trap instruction. */
            int id = g_label_counter++;
            char trap_label[MAX_SYMLEN], ok_label[MAX_SYMLEN];
            snprintf(trap_label, sizeof(trap_label), ".Lassert%d_trap", id);
            snprintf(ok_label, sizeof(ok_label), ".Lassert%d_ok", id);
            static const char *jmp_mnemonic[] = {
                [OP_JE]="b.eq", [OP_JNE]="b.ne", [OP_JG]="b.gt", [OP_JL]="b.lt",
                [OP_JGE]="b.ge", [OP_JLE]="b.le", [OP_JA]="b.hi", [OP_JB]="b.lo",
                [OP_JAE]="b.hs", [OP_JBE]="b.ls"
            };
            fprintf(out, "    %s %s\n", jmp_mnemonic[ins->assert_jmp_op], trap_label);
            fprintf(out, "    b %s\n", ok_label);
            fprintf(out, "%s:\n", trap_label);
            fprintf(out, "    brk #0\n");
            fprintf(out, "%s:\n", ok_label);
            break;
        }

        case OP_CALL: fprintf(out, ins->dst.kind == OPND_REG ? "    blr %s\n" : "    bl %s\n", ins->dst.kind == OPND_REG ? reg__name(TARGET_AARCH64, &ins->dst) : ins->dst.sym); break;
        case OP_RET:  fprintf(out, "    ret\n"); break;

        case OP_EXIT:
            /* src is either OPND_IMM (exit(N)) or OPND_REG (exit(rN)) --
               see parse-time check above. Register case uses 'mov'
               (register-to-register) instead of 'mov #imm'. */
            if (g_libc_linked) {
                /* See the x86-64 OP_EXIT comment: must go through
                   libc's exit() so stdio gets flushed, not the raw
                   exit syscall. */
                if (ins->src.kind == OPND_REG)
                    fprintf(out, "    mov x0, %s\n", reg__name(TARGET_AARCH64, &ins->src));
                else
                    fprintf(out, "    mov x0, #%ld\n", ins->src.imm);
                fprintf(out, "    bl exit\n");
            } else {
                if (ins->src.kind == OPND_REG)
                    fprintf(out, "    mov x0, %s\n", reg__name(TARGET_AARCH64, &ins->src));
                else
                    fprintf(out, "    mov x0, #%ld\n", ins->src.imm);
                fprintf(out, "    mov x8, #93\n");
                fprintf(out, "    svc #0\n");
            }
            break;

        case OP_HALT:
            /* 'wfi' suspends the core until an event/interrupt wakes
               it; wrapped in its own label+branch loop for the same
               reason as x86-64's 'hlt' loop above -- a spurious wake
               falls back into another 'wfi' instead of falling
               through into whatever comes next in memory. */
            {
                int id = g_label_counter++;
                fprintf(out, ".Lhalt%d:\n", id);
                fprintf(out, "    wfi\n");
                fprintf(out, "    b .Lhalt%d\n", id);
            }
            break;

        case OP_STDOUT:
            /* write(1, msg, msg_len). For a global ascii symbol, length
               comes from the compiler-generated <name>_len symbol,
               visible in this same file. For a 'local ascii' buffer
               (src.kind == OPND_LOCAL), msg_len is instead a second
               real local (dst, set up by out()'s parsing) -- its
               address is computed the same fp-relative way OP_LEA
               already computes any local's address (aarch64_local_base
               chases frames_up saved-fp links via SCRATCH_ARM/x12,
               which x1/x2 below never alias, so it's safe to target
               them directly as the accumulator in
               aarch64_emit_local_addr). */
            fprintf(out, "    mov x0, #1\n");
            if (ins->src.kind == OPND_LOCAL) {
                const char *base = aarch64_local_base(out, &ins->src);
                aarch64_emit_local_addr(out, "x1", base, ins->src.local_offset);
                const char *lenbase = aarch64_local_base(out, &ins->dst);
                aarch64_emit_local_addr(out, "x2", lenbase, ins->dst.local_offset);
                fprintf(out, "    ldr x2, [x2]\n");
            } else {
                fprintf(out, "    adrp x1, %s\n", ins->src.sym);
                fprintf(out, "    add x1, x1, :lo12:%s\n", ins->src.sym);
                fprintf(out, "    adrp x2, %s_len\n", ins->src.sym);
                fprintf(out, "    add x2, x2, :lo12:%s_len\n", ins->src.sym);
                fprintf(out, "    ldr x2, [x2]\n");
            }
            fprintf(out, "    mov x8, #64\n"); /* Linux AArch64 write syscall number */
            fprintf(out, "    svc #0\n");
            break;

        case OP_ALLOC: {
            /* Bump allocator: dst = __heap_ptr (the block base); then
               __heap_ptr += size. x13 is used as a second scratch here
               (SCRATCH_ARM/x12 already holds the loaded pointer/size in
               various branches below, so they can't alias). */
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            fprintf(out, "    adrp x13, %s\n", HEAP_PTR_SYM);
            fprintf(out, "    add x13, x13, :lo12:%s\n", HEAP_PTR_SYM);
            fprintf(out, "    ldr %s, [x13]\n", dstreg);
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                fprintf(out, "    add %s, %s, %s\n", SCRATCH_ARM, dstreg, SCRATCH_ARM);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    add %s, %s, #%ld\n", SCRATCH_ARM, dstreg, ins->src.imm);
            } else {
                fprintf(out, "    add %s, %s, %s\n", SCRATCH_ARM, dstreg, reg__name(TARGET_AARCH64, &ins->src));
            }
            fprintf(out, "    str %s, [x13]\n", SCRATCH_ARM);
            break;
        }

        case OP_HEAP_RESET:
            /* Reclaim the whole arena: __heap_ptr = &__heap. x13/x12
               are used the same way OP_ALLOC above uses them: x13 holds
               __heap_ptr's own address (so it can be written to), x12
               (SCRATCH_ARM) holds the value being stored through it. */
            fprintf(out, "    adrp x13, %s\n", HEAP_PTR_SYM);
            fprintf(out, "    add x13, x13, :lo12:%s\n", HEAP_PTR_SYM);
            fprintf(out, "    adrp %s, %s\n", SCRATCH_ARM, HEAP_SYM);
            fprintf(out, "    add %s, %s, :lo12:%s\n", SCRATCH_ARM, SCRATCH_ARM, HEAP_SYM);
            fprintf(out, "    str %s, [x13]\n", SCRATCH_ARM);
            break;

        case OP_ILOAD: {
            /* rDST = *(base + idx*elem_size). AArch64's register-offset
               addressing mode ([Xn, Xm, LSL #n]) applies the scale
               directly, same idea as x86's [base + idx*scale]. See the
               OP_LOAD case above for the sign-extending mnemonic/
               destination-register reasoning (ldrsb/ldrsh/ldrsw vs
               ldrb/ldrh/ldr, and why a 32-bit sign-extend needs the
               full 64-bit dstreg while everything else uses wreg). */
            const char *basereg = reg__name(TARGET_AARCH64, &ins->base_reg);
            const char *idxreg = reg__name(TARGET_AARCH64, &ins->idx_reg);
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            char wreg[16];
            snprintf(wreg, sizeof(wreg), "w%s", dstreg + 1);
            int sz = ins->elem_size;
            const char *mn1 = ins->load_signed ? "ldrsb" : "ldrb";
            const char *mn2 = ins->load_signed ? "ldrsh" : "ldrh";
            const char *mn4 = ins->load_signed ? "ldrsw" : "ldr";
            const char *reg4 = ins->load_signed ? dstreg : wreg;
            if (sz == 1) fprintf(out, "    %s %s, [%s, %s]\n", mn1, wreg, basereg, idxreg);
            else if (sz == 2) fprintf(out, "    %s %s, [%s, %s, lsl #1]\n", mn2, wreg, basereg, idxreg);
            else if (sz == 4) fprintf(out, "    %s %s, [%s, %s, lsl #2]\n", mn4, reg4, basereg, idxreg);
            else fprintf(out, "    ldr %s, [%s, %s, lsl #3]\n", dstreg, basereg, idxreg);
            break;
        }

        case OP_BCMP: {
            /* bcmpN rDST, rPTR1, rPTR2, LEN -- see the x86-64 OP_BCMP
               case for the overall two-loop (elem_size chunks, then a
               byte tail) strategy; this is the same shape adapted to
               AArch64's load/store-only addressing. x12/x13 are the
               byte offset and remaining-length countdown (never
               rPTR1/rPTR2/rDST's own registers, which could be any of
               x0-x11); x14/x15 hold each side's loaded value in turn. */
            const char *p1reg = reg__name(TARGET_AARCH64, &ins->src);
            const char *p2reg = reg__name(TARGET_AARCH64, &ins->base_reg);
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            int sz = ins->elem_size;

            char id[16];
            snprintf(id, sizeof(id), "%d", g_label_counter++);
            char chunk_lbl[40], chunkcheck_lbl[40], chunkskip_lbl[40];
            char tail_lbl[40], tailcheck_lbl[40], tailskip_lbl[40], done_lbl[40];
            snprintf(chunk_lbl, sizeof(chunk_lbl), ".Lbcmp_chunk%s", id);
            snprintf(chunkcheck_lbl, sizeof(chunkcheck_lbl), ".Lbcmp_chunkcheck%s", id);
            snprintf(chunkskip_lbl, sizeof(chunkskip_lbl), ".Lbcmp_chunkskip%s", id);
            snprintf(tail_lbl, sizeof(tail_lbl), ".Lbcmp_tail%s", id);
            snprintf(tailcheck_lbl, sizeof(tailcheck_lbl), ".Lbcmp_tailcheck%s", id);
            snprintf(tailskip_lbl, sizeof(tailskip_lbl), ".Lbcmp_tailskip%s", id);
            snprintf(done_lbl, sizeof(done_lbl), ".Lbcmp_done%s", id);

            fprintf(out, "    mov %s, #0\n", dstreg); /* assume equal */
            fprintf(out, "    mov x12, #0\n"); /* byte offset, shared by both loops */
            if (ins->len_reg.kind == OPND_IMM) {
                fprintf(out, "    mov x13, #%ld\n", ins->len_reg.imm);
            } else {
                fprintf(out, "    mov x13, %s\n", reg__name(TARGET_AARCH64, &ins->len_reg));
            }

            if (sz > 1) {
                fprintf(out, "    b %s\n", chunkcheck_lbl);
                fprintf(out, "%s:\n", chunk_lbl);
                if (sz == 2) {
                    fprintf(out, "    ldrh w14, [%s, x12]\n", p1reg);
                    fprintf(out, "    ldrh w15, [%s, x12]\n", p2reg);
                } else if (sz == 4) {
                    fprintf(out, "    ldr w14, [%s, x12]\n", p1reg);
                    fprintf(out, "    ldr w15, [%s, x12]\n", p2reg);
                } else {
                    fprintf(out, "    ldr x14, [%s, x12]\n", p1reg);
                    fprintf(out, "    ldr x15, [%s, x12]\n", p2reg);
                }
                fprintf(out, "    cmp x14, x15\n");
                fprintf(out, "    b.eq %s\n", chunkskip_lbl);
                fprintf(out, "    mov %s, #1\n", dstreg);
                fprintf(out, "%s:\n", chunkskip_lbl);
                fprintf(out, "    add x12, x12, #%d\n", sz);
                fprintf(out, "    sub x13, x13, #%d\n", sz);
                fprintf(out, "%s:\n", chunkcheck_lbl);
                fprintf(out, "    cmp x13, #%d\n", sz);
                fprintf(out, "    b.ge %s\n", chunk_lbl);
            }

            fprintf(out, "    b %s\n", tailcheck_lbl);
            fprintf(out, "%s:\n", tail_lbl);
            fprintf(out, "    ldrb w14, [%s, x12]\n", p1reg);
            fprintf(out, "    ldrb w15, [%s, x12]\n", p2reg);
            fprintf(out, "    cmp w14, w15\n");
            fprintf(out, "    b.eq %s\n", tailskip_lbl);
            fprintf(out, "    mov %s, #1\n", dstreg);
            fprintf(out, "%s:\n", tailskip_lbl);
            fprintf(out, "    add x12, x12, #1\n");
            fprintf(out, "    sub x13, x13, #1\n");
            fprintf(out, "%s:\n", tailcheck_lbl);
            fprintf(out, "    cmp x13, #0\n");
            fprintf(out, "    b.gt %s\n", tail_lbl);
            fprintf(out, "%s:\n", done_lbl);
            break;
        }

        case OP_BCOPY: {
            /* bcopyN rDST, rSRC, LEN -- AArch64 counterpart to the
               x86-64 OP_BCOPY case (see its comment for the overall
               strategy relative to bcmpN); adapted the same way
               OP_BCMP above is adapted from its own x86-64 case. x12
               is the shared byte offset, x13 the spilled countdown
               (kept in a register here rather than on the stack, same
               as bcmpN does on this target), x14 stages each chunk's
               value between the load from rSRC and the store to
               rDST. */
            const char *srcreg = reg__name(TARGET_AARCH64, &ins->src);
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            int sz = ins->elem_size;

            char id[16];
            snprintf(id, sizeof(id), "%d", g_label_counter++);
            char chunk_lbl[40], chunkcheck_lbl[40];
            char tail_lbl[40], tailcheck_lbl[40], done_lbl[40];
            snprintf(chunk_lbl, sizeof(chunk_lbl), ".Lbcopy_chunk%s", id);
            snprintf(chunkcheck_lbl, sizeof(chunkcheck_lbl), ".Lbcopy_chunkcheck%s", id);
            snprintf(tail_lbl, sizeof(tail_lbl), ".Lbcopy_tail%s", id);
            snprintf(tailcheck_lbl, sizeof(tailcheck_lbl), ".Lbcopy_tailcheck%s", id);
            snprintf(done_lbl, sizeof(done_lbl), ".Lbcopy_done%s", id);

            fprintf(out, "    mov x12, #0\n"); /* byte offset, shared by both loops */
            if (ins->len_reg.kind == OPND_IMM) {
                fprintf(out, "    mov x13, #%ld\n", ins->len_reg.imm);
            } else {
                fprintf(out, "    mov x13, %s\n", reg__name(TARGET_AARCH64, &ins->len_reg));
            }

            if (sz > 1) {
                fprintf(out, "    b %s\n", chunkcheck_lbl);
                fprintf(out, "%s:\n", chunk_lbl);
                if (sz == 2) {
                    fprintf(out, "    ldrh w14, [%s, x12]\n", srcreg);
                    fprintf(out, "    strh w14, [%s, x12]\n", dstreg);
                } else if (sz == 4) {
                    fprintf(out, "    ldr w14, [%s, x12]\n", srcreg);
                    fprintf(out, "    str w14, [%s, x12]\n", dstreg);
                } else {
                    fprintf(out, "    ldr x14, [%s, x12]\n", srcreg);
                    fprintf(out, "    str x14, [%s, x12]\n", dstreg);
                }
                fprintf(out, "    add x12, x12, #%d\n", sz);
                fprintf(out, "    sub x13, x13, #%d\n", sz);
                fprintf(out, "%s:\n", chunkcheck_lbl);
                fprintf(out, "    cmp x13, #%d\n", sz);
                fprintf(out, "    b.ge %s\n", chunk_lbl);
            }

            fprintf(out, "    b %s\n", tailcheck_lbl);
            fprintf(out, "%s:\n", tail_lbl);
            fprintf(out, "    ldrb w14, [%s, x12]\n", srcreg);
            fprintf(out, "    strb w14, [%s, x12]\n", dstreg);
            fprintf(out, "    add x12, x12, #1\n");
            fprintf(out, "    sub x13, x13, #1\n");
            fprintf(out, "%s:\n", tailcheck_lbl);
            fprintf(out, "    cmp x13, #0\n");
            fprintf(out, "    b.gt %s\n", tail_lbl);
            fprintf(out, "%s:\n", done_lbl);
            break;
        }


        case OP_ISTORE: {
            const char *basereg = reg__name(TARGET_AARCH64, &ins->base_reg);
            const char *idxreg = reg__name(TARGET_AARCH64, &ins->idx_reg);
            int sz = ins->elem_size;
            const char *srcreg;
            char wreg[16];
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                srcreg = SCRATCH_ARM;
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
                srcreg = SCRATCH_ARM;
            } else {
                srcreg = reg__name(TARGET_AARCH64, &ins->src);
            }
            snprintf(wreg, sizeof(wreg), "w%s", srcreg + 1);
            if (sz == 1) fprintf(out, "    strb %s, [%s, %s]\n", wreg, basereg, idxreg);
            else if (sz == 2) fprintf(out, "    strh %s, [%s, %s, lsl #1]\n", wreg, basereg, idxreg);
            else if (sz == 4) fprintf(out, "    str %s, [%s, %s, lsl #2]\n", wreg, basereg, idxreg);
            else fprintf(out, "    str %s, [%s, %s, lsl #3]\n", srcreg, basereg, idxreg);
            break;
        }

        case OP_HFIELD_LOAD: {
            /* rDST = *(rBASE + const_offset). AArch64's immediate-offset
               addressing ([Xn, #imm]) applies the constant directly --
               no register-scaled index needed here, unlike OP_ILOAD's
               [Xn, Xm, LSL #n]. */
            const char *basereg = reg__name(TARGET_AARCH64, &ins->base_reg);
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            char wreg[16];
            snprintf(wreg, sizeof(wreg), "w%s", dstreg + 1);
            int sz = ins->elem_size;
            long safe_off;
            /* basereg is a caller-supplied pointer register (r1-r12,
               never x12/x13) that must survive this instruction, so it
               can't be clobbered -- materialize into x13 if the field
               offset doesn't fit (x12/SCRATCH_ARM isn't touched here,
               so it's free too, but x13 keeps this consistent with the
               other "basereg must survive" call sites below). */
            const char *addrbase = aarch64_safe_offset(out, basereg, (long)ins->const_offset, sz, "x13", &safe_off);
            if (sz == 1) fprintf(out, "    ldrb %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else if (sz == 2) fprintf(out, "    ldrh %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else if (sz == 4) fprintf(out, "    ldr %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else fprintf(out, "    ldr %s, [%s, #%ld]\n", dstreg, addrbase, safe_off);
            break;
        }

        case OP_HFIELD_STORE: {
            const char *basereg = reg__name(TARGET_AARCH64, &ins->base_reg);
            int sz = ins->elem_size;
            const char *srcreg;
            char wreg[16];
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                srcreg = SCRATCH_ARM;
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
                srcreg = SCRATCH_ARM;
            } else {
                srcreg = reg__name(TARGET_AARCH64, &ins->src);
            }
            snprintf(wreg, sizeof(wreg), "w%s", srcreg + 1);
            long safe_off;
            /* srcreg may already be SCRATCH_ARM (x12, from the
               mem/imm branches above) holding the value to store, so
               address materialization must not clobber x12 -- use x13
               instead, same reasoning as OP_HFIELD_LOAD. */
            const char *addrbase = aarch64_safe_offset(out, basereg, (long)ins->const_offset, sz, "x13", &safe_off);
            if (sz == 1) fprintf(out, "    strb %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else if (sz == 2) fprintf(out, "    strh %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else if (sz == 4) fprintf(out, "    str %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else fprintf(out, "    str %s, [%s, #%ld]\n", srcreg, addrbase, safe_off);
            break;
        }

        case OP_XLOAD: {
            /* rDST = *(base + idx*scale + disp). AArch64 has no single
               instruction combining a scaled-register index AND an
               immediate displacement (unlike x86's SIB-based [base +
               idx*scale + disp], see the x86-64 OP_XLOAD case) -- so
               this computes base+idx*scale into x12 first (AArch64's
               'add Xd, Xn, Xm, LSL #n' extended-register form handles
               the scale directly, same shift-amount-from-elem_size
               idea OP_ILOAD already uses), then applies disp via
               aarch64_safe_offset on top of that, the same helper
               OP_HFIELD_LOAD uses for its own displacement-only case.
               x13 is aarch64_safe_offset's own materialization
               register when disp doesn't fit an immediate -- distinct
               from x12 here, so the two computations can't clobber
               each other. */
            const char *basereg = reg__name(TARGET_AARCH64, &ins->base_reg);
            const char *idxreg = reg__name(TARGET_AARCH64, &ins->idx_reg);
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            char wreg[16];
            snprintf(wreg, sizeof(wreg), "w%s", dstreg + 1);
            int sz = ins->elem_size;
            int scale = ins->xaddr_scale;
            int shift = scale == 1 ? 0 : scale == 2 ? 1 : scale == 4 ? 2 : 3;
            if (shift > 0) fprintf(out, "    add %s, %s, %s, lsl #%d\n", SCRATCH_ARM, basereg, idxreg, shift);
            else fprintf(out, "    add %s, %s, %s\n", SCRATCH_ARM, basereg, idxreg);
            long safe_off;
            const char *addrbase = aarch64_safe_offset(out, SCRATCH_ARM, (long)ins->const_offset, sz, "x13", &safe_off);
            if (sz == 1) fprintf(out, "    ldrb %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else if (sz == 2) fprintf(out, "    ldrh %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else if (sz == 4) fprintf(out, "    ldr %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else fprintf(out, "    ldr %s, [%s, #%ld]\n", dstreg, addrbase, safe_off);
            break;
        }

        case OP_XSTORE: {
            const char *basereg = reg__name(TARGET_AARCH64, &ins->base_reg);
            const char *idxreg = reg__name(TARGET_AARCH64, &ins->idx_reg);
            int sz = ins->elem_size;
            int scale = ins->xaddr_scale;
            int shift = scale == 1 ? 0 : scale == 2 ? 1 : scale == 4 ? 2 : 3;
            /* base+idx*scale is computed into x12 (SCRATCH_ARM) before
               the source value is materialized, unlike OP_HFIELD_STORE
               (which computes srcreg first) -- here the address
               computation itself needs x12, so if src also needed a
               scratch it would collide. Materializing src into x14
               (never used elsewhere in this case) instead of x12 sidesteps
               that ordering dependency entirely. */
            const char *srcreg;
            char wreg[16];
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                fprintf(out, "    mov x14, %s\n", SCRATCH_ARM);
                srcreg = "x14";
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov x14, #%ld\n", ins->src.imm);
                srcreg = "x14";
            } else {
                srcreg = reg__name(TARGET_AARCH64, &ins->src);
            }
            snprintf(wreg, sizeof(wreg), "w%s", srcreg + 1);
            if (shift > 0) fprintf(out, "    add %s, %s, %s, lsl #%d\n", SCRATCH_ARM, basereg, idxreg, shift);
            else fprintf(out, "    add %s, %s, %s\n", SCRATCH_ARM, basereg, idxreg);
            long safe_off;
            const char *addrbase = aarch64_safe_offset(out, SCRATCH_ARM, (long)ins->const_offset, sz, "x13", &safe_off);
            if (sz == 1) fprintf(out, "    strb %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else if (sz == 2) fprintf(out, "    strh %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else if (sz == 4) fprintf(out, "    str %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
            else fprintf(out, "    str %s, [%s, #%ld]\n", srcreg, addrbase, safe_off);
            break;
        }

        case OP_PTRADD: case OP_PTRSUB: {
            /* rDST = base +/- (idx*scale + disp), or rDST = base +/-
               disp when idx_reg is absent (the bracket-less form --
               see its parse-time comment). Builds the address into
               dstreg directly (never dereferenced), reusing the same
               'add Xd, Xn, Xm, LSL #n' scaled-index trick OP_XLOAD
               already uses for the idx*scale term, then applies disp
               with the same 12-bit+lsl#12 immediate-splitting
               aarch64_safe_offset uses internally for an out-of-range
               displacement -- inlined here rather than calling that
               helper directly, since aarch64_safe_offset is shaped for
               "[base, #offset]" load/store addressing (it returns a
               base register and a load/store-legal residual offset),
               not "compute base+offset into a plain destination
               register", which is what an address-only op needs. */
            const char *basereg = reg__name(TARGET_AARCH64, &ins->base_reg);
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            int is_sub = (ins->op == OP_PTRSUB);
            long disp = is_sub ? -(long)ins->const_offset : (long)ins->const_offset;

            const char *addr_src = basereg; /* what disp still needs to be added to */
            if (ins->idx_reg.kind == OPND_REG) {
                const char *idxreg = reg__name(TARGET_AARCH64, &ins->idx_reg);
                int scale = ins->xaddr_scale;
                int shift = scale == 1 ? 0 : scale == 2 ? 1 : scale == 4 ? 2 : 3;
                if (!is_sub) {
                    if (shift > 0) fprintf(out, "    add %s, %s, %s, lsl #%d\n", dstreg, basereg, idxreg, shift);
                    else fprintf(out, "    add %s, %s, %s\n", dstreg, basereg, idxreg);
                } else {
                    if (shift > 0) fprintf(out, "    sub %s, %s, %s, lsl #%d\n", dstreg, basereg, idxreg, shift);
                    else fprintf(out, "    sub %s, %s, %s\n", dstreg, basereg, idxreg);
                }
                addr_src = dstreg; /* disp now applies on top of dstreg, not basereg */
            }

            if (disp != 0) {
                long mag = disp < 0 ? -disp : disp;
                const char *mn = disp < 0 ? "sub" : "add";
                long lo = mag & 0xFFF;
                long hi = mag >> 12;
                if (hi > 0xFFF) {
                    g_source_line = NULL;
                    fail_fmt("internal error: ptradd/ptrsub displacement %ld exceeds AArch64 addressable range", disp);
                }
                if (hi > 0) fprintf(out, "    %s %s, %s, #%ld, lsl #12\n", mn, dstreg, addr_src, hi);
                if (lo > 0 || hi == 0) fprintf(out, "    %s %s, %s, #%ld\n", mn, dstreg, hi > 0 ? dstreg : addr_src, lo);
            } else if (addr_src != dstreg) {
                /* No index term and no displacement: dst is just a
                   plain copy of base. */
                fprintf(out, "    mov %s, %s\n", dstreg, basereg);
            }
            break;
        }

        case OP_LALOAD: {
            /* rDST = local array element at idx. AArch64's register-
               offset addressing ([Xn, Xm, LSL #n]) needs a register
               base, unlike x86's '[rbp - N + idx*scale]' text form, so
               the array's fixed base (x29 minus its compile-time
               offset, chasing the saved-fp chain first if frames_up >
               0, same as any other local access) is computed into
               scratch first, then indexed the same way OP_ILOAD indexes
               a heap array. A literal index folds directly into an
               immediate offset instead, skipping the register-scaled
               form entirely since there's nothing to scale at runtime
               in that case. */
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            char wreg[16];
            snprintf(wreg, sizeof(wreg), "w%s", dstreg + 1);
            int sz = ins->elem_size;

            const char *fpreg = "x29";
            if (ins->src.frames_up > 0) {
                fprintf(out, "    ldr %s, [x29]\n", SCRATCH_ARM);
                for (int k = 1; k < ins->src.frames_up; k++)
                    fprintf(out, "    ldr %s, [%s]\n", SCRATCH_ARM, SCRATCH_ARM);
                fpreg = SCRATCH_ARM;
            }

            if (ins->idx_reg.kind == OPND_IMM) {
                long total_off = ins->src.local_offset - ins->idx_reg.imm * sz;
                long safe_off;
                /* dstreg is being written by this load, so it's safe
                   to reuse as address scratch if the offset needs
                   materializing -- same reasoning as plain OP_LOAD. */
                const char *addrbase = aarch64_safe_offset(out, fpreg, -total_off, sz, dstreg, &safe_off);
                if (sz == 1) fprintf(out, "    ldrb %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
                else if (sz == 2) fprintf(out, "    ldrh %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
                else if (sz == 4) fprintf(out, "    ldr %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
                else fprintf(out, "    ldr %s, [%s, #%ld]\n", dstreg, addrbase, safe_off);
            } else {
                /* fpreg may already be SCRATCH_ARM (x12) from the fp
                   chase above; the address-of-array-base sub must land
                   somewhere that doesn't collide with fpreg while it's
                   still being read, so fall back to x13 in that case
                   (mirrors OP_LASTORE's addrreg selection below). */
                const char *addrreg = (strcmp(fpreg, SCRATCH_ARM) == 0) ? "x13" : SCRATCH_ARM;
                long mag = ins->src.local_offset;
                long lo = mag & 0xFFF;
                long hi = mag >> 12;
                if (hi > 0xFFF) {
                    g_source_line = NULL;
                    fail_fmt("internal error: local offset %d exceeds AArch64 addressable range", ins->src.local_offset);
                }
                if (hi > 0) {
                    fprintf(out, "    sub %s, %s, #%ld, lsl #12\n", addrreg, fpreg, hi);
                    if (lo > 0) fprintf(out, "    sub %s, %s, #%ld\n", addrreg, addrreg, lo);
                } else {
                    fprintf(out, "    sub %s, %s, #%ld\n", addrreg, fpreg, lo);
                }
                const char *idxreg = reg__name(TARGET_AARCH64, &ins->idx_reg);
                if (sz == 1) fprintf(out, "    ldrb %s, [%s, %s]\n", wreg, addrreg, idxreg);
                else if (sz == 2) fprintf(out, "    ldrh %s, [%s, %s, lsl #1]\n", wreg, addrreg, idxreg);
                else if (sz == 4) fprintf(out, "    ldr %s, [%s, %s, lsl #2]\n", wreg, addrreg, idxreg);
                else fprintf(out, "    ldr %s, [%s, %s, lsl #3]\n", dstreg, addrreg, idxreg);
            }
            break;
        }

        case OP_LASTORE: {
            int sz = ins->elem_size;
            const char *srcreg;
            char wreg[16];
            if (is_mem_operand(ins->src.kind)) {
                emit_aarch64_load_scratch(out, &ins->src);
                srcreg = SCRATCH_ARM;
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
                srcreg = SCRATCH_ARM;
            } else {
                srcreg = reg__name(TARGET_AARCH64, &ins->src);
            }
            snprintf(wreg, sizeof(wreg), "w%s", srcreg + 1);

            /* srcreg may already be SCRATCH_ARM (x12) above; the fp
               chase below also wants SCRATCH_ARM, which would clobber
               an immediate/mem-operand source before it's stored. Move
               such a source into x13 first so the two uses don't
               collide -- a register-operand source is unaffected since
               it's never SCRATCH_ARM to begin with (Chard's r1-r12 never
               map to x12). */
            if (ins->src.kind == OPND_IMM || is_mem_operand(ins->src.kind)) {
                fprintf(out, "    mov x13, %s\n", SCRATCH_ARM);
                srcreg = "x13";
                snprintf(wreg, sizeof(wreg), "w13");
            }

            const char *fpreg = "x29";
            if (ins->dst.frames_up > 0) {
                fprintf(out, "    ldr %s, [x29]\n", SCRATCH_ARM);
                for (int k = 1; k < ins->dst.frames_up; k++)
                    fprintf(out, "    ldr %s, [%s]\n", SCRATCH_ARM, SCRATCH_ARM);
                fpreg = SCRATCH_ARM;
            }

            if (ins->idx_reg.kind == OPND_IMM) {
                long total_off = ins->dst.local_offset - ins->idx_reg.imm * sz;
                long safe_off;
                /* srcreg/wreg may already occupy x12 or x13 (see the
                   comment above), so address materialization here uses
                   x14, which nothing in this case touches otherwise. */
                const char *addrbase = aarch64_safe_offset(out, fpreg, -total_off, sz, "x14", &safe_off);
                if (sz == 1) fprintf(out, "    strb %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
                else if (sz == 2) fprintf(out, "    strh %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
                else if (sz == 4) fprintf(out, "    str %s, [%s, #%ld]\n", wreg, addrbase, safe_off);
                else fprintf(out, "    str %s, [%s, #%ld]\n", srcreg, addrbase, safe_off);
            } else {
                const char *addrreg = (strcmp(fpreg, SCRATCH_ARM) == 0) ? "x13" : SCRATCH_ARM;
                long mag = ins->dst.local_offset;
                long lo = mag & 0xFFF;
                long hi = mag >> 12;
                if (hi > 0xFFF) {
                    g_source_line = NULL;
                    fail_fmt("internal error: local offset %d exceeds AArch64 addressable range", ins->dst.local_offset);
                }
                if (hi > 0) {
                    fprintf(out, "    sub %s, %s, #%ld, lsl #12\n", addrreg, fpreg, hi);
                    if (lo > 0) fprintf(out, "    sub %s, %s, #%ld\n", addrreg, addrreg, lo);
                } else {
                    fprintf(out, "    sub %s, %s, #%ld\n", addrreg, fpreg, lo);
                }
                const char *idxreg = reg__name(TARGET_AARCH64, &ins->idx_reg);
                if (sz == 1) fprintf(out, "    strb %s, [%s, %s]\n", wreg, addrreg, idxreg);
                else if (sz == 2) fprintf(out, "    strh %s, [%s, %s, lsl #1]\n", wreg, addrreg, idxreg);
                else if (sz == 4) fprintf(out, "    str %s, [%s, %s, lsl #2]\n", wreg, addrreg, idxreg);
                else fprintf(out, "    str %s, [%s, %s, lsl #3]\n", srcreg, addrreg, idxreg);
            }
            break;
        }

        case OP_PUSH:
            /* AArch64 has no push instruction; a pre-indexed store to
               [sp, #-N]! is the standard idiom, where N comes from
               stack_slot_size() (see the table up top) rather than
               being repeated here as a literal. */
            if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
                fprintf(out, "    str %s, [sp, #-%d]!\n", SCRATCH_ARM, stack_slot_size(TARGET_AARCH64));
            } else {
                fprintf(out, "    str %s, [sp, #-%d]!\n", reg__name(TARGET_AARCH64, &ins->src), stack_slot_size(TARGET_AARCH64));
            }
            break;

        case OP_POP:
            /* Mirrors OP_PUSH: post-indexed load from sp, then sp
               advances by the same stack_slot_size() amount the push
               side used to decrement it. */
            fprintf(out, "    ldr %s, [sp], #%d\n",
                    ins->dst.kind == OPND_REG ? reg__name(TARGET_AARCH64, &ins->dst) : SCRATCH_ARM,
                    stack_slot_size(TARGET_AARCH64));
            break;

        case OP_ATOMIC_ADD: case OP_ATOMIC_SUB: case OP_ATOMIC_AND:
        case OP_ATOMIC_OR: case OP_ATOMIC_XOR: case OP_ATOMIC_SWAP: {
            /* Load-linked/store-conditional (LL/SC) retry loop -- works
               on every ARMv8.0+ core, unlike single-instruction LSE
               atomics (ldaddal etc, ARMv8.1+) which aren't guaranteed
               present. Register roles, fixed for the loop:
                 x12 (SCRATCH_ARM) - target address, computed once
                                     before the loop
                 result_reg        - holds the freshly-read 'old' value
                                     each pass, copied immediately after
                                     the load before x13 is overwritten
                 x13                - scratch: loaded value, then the
                                     computed new value to store
                 w14                - stxr's exclusive-store status
                                     (0 = succeeded, nonzero = retry)
               SRC must be a register or immediate (not memory): LL/SC
               needs a tight load/modify/store window with no other
               memory access in between, or store-exclusive can
               spuriously fail forever on some implementations.

               Ordering (ins->mem_order) picks which of ldxr/ldaxr and
               stxr/stlxr this loop uses (AArch64 acquire/release are
               separate instructions, not a suffix bit):
                 SEQ_CST/ACQ_REL -> ldaxr + stlxr (acq+rel on every RMW
                                     access is seq-cst on ARMv8; also
                                     the pre-existing pair used before
                                     the ordering parameter existed)
                 ACQUIRE         -> ldaxr + stxr (only the read barriers)
                 RELEASE         -> ldxr + stlxr (only the write barriers)
                 RELAXED         -> ldxr + stxr (no barrier) */
            int sz = operand_mem_size(&ins->dst);
            int use_acquire = ins->mem_order == MEM_ORDER_SEQ_CST || ins->mem_order == MEM_ORDER_ACQ_REL || ins->mem_order == MEM_ORDER_ACQUIRE;
            int use_release = ins->mem_order == MEM_ORDER_SEQ_CST || ins->mem_order == MEM_ORDER_ACQ_REL || ins->mem_order == MEM_ORDER_RELEASE;
            const char *ldx = sz == 1 ? (use_acquire ? "ldaxrb" : "ldxrb")
                             : sz == 2 ? (use_acquire ? "ldaxrh" : "ldxrh")
                             :           (use_acquire ? "ldaxr"  : "ldxr");
            const char *stx = sz == 1 ? (use_release ? "stlxrb" : "stxrb")
                             : sz == 2 ? (use_release ? "stlxrh" : "stxrh")
                             :           (use_release ? "stlxr"  : "stxr");
            const char *valreg13 = sz == 8 ? "x13" : "w13";
            const char *resultreg = sz == 8 ? reg__name(TARGET_AARCH64, &ins->result_reg)
                                             : width_reg_name(TARGET_AARCH64, &ins->result_reg, 4);
            const char *addrreg = emit_aarch64_addr_into_scratch(out, &ins->dst);

            char srcval[32];
            render_simple_operand(TARGET_AARCH64, &ins->src, srcval, sizeof(srcval));

            char loop_lbl[32];
            snprintf(loop_lbl, sizeof(loop_lbl), ".Latomic_retry%d", g_label_counter++);

            fprintf(out, "%s:\n", loop_lbl);
            fprintf(out, "    %s %s, [%s]\n", ldx, valreg13, addrreg);
            fprintf(out, "    mov %s, %s\n", resultreg, valreg13); /* stash old value before it's overwritten below */
            if (ins->op != OP_ATOMIC_SWAP) {
                const char *mn = ins->op == OP_ATOMIC_ADD ? "add" : ins->op == OP_ATOMIC_SUB ? "sub" :
                                  ins->op == OP_ATOMIC_AND ? "and" : ins->op == OP_ATOMIC_OR ? "orr" : "eor";
                fprintf(out, "    %s %s, %s, %s\n", mn, valreg13, valreg13, srcval);
            } else {
                fprintf(out, "    mov %s, %s\n", valreg13, srcval);
            }
            fprintf(out, "    %s w14, %s, [%s]\n", stx, valreg13, addrreg);
            fprintf(out, "    cbnz w14, %s\n", loop_lbl);
            break;
        }

        case OP_ATOMIC_MAX: case OP_ATOMIC_MIN: {
            /* Same LL/SC retry shape as the ADD/SUB/AND/OR/XOR/SWAP
               block above (see its comment for the full register-role
               and ordering rationale, which applies unchanged here) --
               the one addition is a cmp + csel to pick whichever of the
               loaded value / SRC is larger (or smaller) as the value to
               attempt storing, using x15/w15 to hold SRC since w14 is
               already claimed by stxr's status result in this loop.
               Signed comparison (csel's gt/lt condition codes), matching
               the opcode_t comment on OP_ATOMIC_MAX/OP_ATOMIC_MIN.

               sz==1/sz==2: ldxrb/ldxrh/ldaxrb/ldaxrh zero-extend the
               loaded byte/halfword into w13 -- with no correction, a
               negative narrow value (e.g. -1 stored as 0xFF) would load
               as a large positive 32-bit number (0x000000FF = 255) and
               compare as such, picking the wrong "winner" and reporting
               the wrong old value in resultreg. sxtb/sxth re-establishes
               the correct sign in w13 immediately after the load, before
               either the old-value stash into resultreg or the cmp/csel
               below see it -- mirroring x86-64's movsx r14d/r15d guard
               for the identical sub-32-bit signed-compare problem (see
               that block's comment). Only the *loaded* value needs this:
               valreg15 (SRC) already comes from either a full-width
               register (Chard has no register narrower than 32 bits) or
               an assembler-sign-extended immediate, so it's correctly
               signed at 32 bits before this block ever runs. No-op for
               sz==8 (sxtb/sxth are only emitted when sz==1/sz==2). */
            int sz = operand_mem_size(&ins->dst);
            int use_acquire = ins->mem_order == MEM_ORDER_SEQ_CST || ins->mem_order == MEM_ORDER_ACQ_REL || ins->mem_order == MEM_ORDER_ACQUIRE;
            int use_release = ins->mem_order == MEM_ORDER_SEQ_CST || ins->mem_order == MEM_ORDER_ACQ_REL || ins->mem_order == MEM_ORDER_RELEASE;
            const char *ldx = sz == 1 ? (use_acquire ? "ldaxrb" : "ldxrb")
                             : sz == 2 ? (use_acquire ? "ldaxrh" : "ldxrh")
                             :           (use_acquire ? "ldaxr"  : "ldxr");
            const char *stx = sz == 1 ? (use_release ? "stlxrb" : "stxrb")
                             : sz == 2 ? (use_release ? "stlxrh" : "stxrh")
                             :           (use_release ? "stlxr"  : "stxr");
            const char *valreg13 = sz == 8 ? "x13" : "w13";
            const char *valreg15 = sz == 8 ? "x15" : "w15";
            const char *resultreg = sz == 8 ? reg__name(TARGET_AARCH64, &ins->result_reg)
                                             : width_reg_name(TARGET_AARCH64, &ins->result_reg, 4);
            const char *addrreg = emit_aarch64_addr_into_scratch(out, &ins->dst);

            char srcval[32];
            render_simple_operand(TARGET_AARCH64, &ins->src, srcval, sizeof(srcval));

            char loop_lbl[32];
            snprintf(loop_lbl, sizeof(loop_lbl), ".Latomic_retry%d", g_label_counter++);

            fprintf(out, "    mov %s, %s\n", valreg15, srcval);
            fprintf(out, "%s:\n", loop_lbl);
            fprintf(out, "    %s %s, [%s]\n", ldx, valreg13, addrreg);
            if (sz == 1) fprintf(out, "    sxtb %s, %s\n", valreg13, valreg13);
            else if (sz == 2) fprintf(out, "    sxth %s, %s\n", valreg13, valreg13);
            fprintf(out, "    mov %s, %s\n", resultreg, valreg13); /* stash old value (now correctly sign-extended) before it's overwritten below */
            fprintf(out, "    cmp %s, %s\n", valreg13, valreg15);
            fprintf(out, "    csel %s, %s, %s, %s\n", valreg13, valreg13, valreg15, ins->op == OP_ATOMIC_MAX ? "gt" : "lt");
            fprintf(out, "    %s w14, %s, [%s]\n", stx, valreg13, addrreg);
            fprintf(out, "    cbnz w14, %s\n", loop_lbl);
            break;
        }

        case OP_ATOMIC_CAS: {
            /* Same LL/SC shape, but the store is conditional on a
               compare rather than unconditional -- ldxr, compare against
               the caller-supplied expected value, and only stxr (attempt
               the commit) if they matched; otherwise clear the exclusive
               monitor with clrex and report failure without ever having
               written anything. result_reg reports 1/0 success, not the
               old value (see the OP_ATOMIC_CAS comment in the opcode_t
               enum for why this convention was chosen). Ordering: same
               ldxr/ldaxr and stxr/stlxr selection as the RMW ops above
               -- see that block's comment for the full ordering-to-
               instruction-pair mapping. */
            int sz = operand_mem_size(&ins->dst);
            int use_acquire = ins->mem_order == MEM_ORDER_SEQ_CST || ins->mem_order == MEM_ORDER_ACQ_REL || ins->mem_order == MEM_ORDER_ACQUIRE;
            int use_release = ins->mem_order == MEM_ORDER_SEQ_CST || ins->mem_order == MEM_ORDER_ACQ_REL || ins->mem_order == MEM_ORDER_RELEASE;
            const char *ldx = sz == 1 ? (use_acquire ? "ldaxrb" : "ldxrb")
                             : sz == 2 ? (use_acquire ? "ldaxrh" : "ldxrh")
                             :           (use_acquire ? "ldaxr"  : "ldxr");
            const char *stx = sz == 1 ? (use_release ? "stlxrb" : "stxrb")
                             : sz == 2 ? (use_release ? "stlxrh" : "stxrh")
                             :           (use_release ? "stlxr"  : "stxr");
            const char *valreg13 = sz == 8 ? "x13" : "w13";
            const char *expreg = sz == 8 ? reg__name(TARGET_AARCH64, &ins->cas_expected)
                                          : width_reg_name(TARGET_AARCH64, &ins->cas_expected, 4);
            const char *desreg = sz == 8 ? reg__name(TARGET_AARCH64, &ins->cas_desired)
                                          : width_reg_name(TARGET_AARCH64, &ins->cas_desired, 4);
            const char *resultreg = reg__name(TARGET_AARCH64, &ins->result_reg);
            const char *addrreg = emit_aarch64_addr_into_scratch(out, &ins->dst);

            char loop_lbl[32], fail_lbl[32], done_lbl[32];
            snprintf(loop_lbl, sizeof(loop_lbl), ".Lcas_retry%d", g_label_counter);
            snprintf(fail_lbl, sizeof(fail_lbl), ".Lcas_fail%d", g_label_counter);
            snprintf(done_lbl, sizeof(done_lbl), ".Lcas_done%d", g_label_counter);
            g_label_counter++;

            fprintf(out, "%s:\n", loop_lbl);
            fprintf(out, "    %s %s, [%s]\n", ldx, valreg13, addrreg);
            fprintf(out, "    cmp %s, %s\n", valreg13, expreg);
            fprintf(out, "    b.ne %s\n", fail_lbl);
            fprintf(out, "    %s w14, %s, [%s]\n", stx, desreg, addrreg);
            fprintf(out, "    cbnz w14, %s\n", loop_lbl); /* lost the reservation racing another core -- retry from the top */
            fprintf(out, "    mov %s, #1\n", resultreg);
            fprintf(out, "    b %s\n", done_lbl);
            fprintf(out, "%s:\n", fail_lbl);
            fprintf(out, "    clrex\n"); /* release the monitor -- no store was attempted, so nothing to retry */
            fprintf(out, "    mov %s, #0\n", resultreg);
            fprintf(out, "%s:\n", done_lbl);
            break;
        }

        case OP_I2S: {
            /* i2s rSRC > rBUF, rLEN. Same digit algorithm as x86-64
               (see its comment for the overall shape), adapted to
               AArch64's load/store-only addressing and its lack of a
               combined div+mod instruction (udiv gives the quotient;
               msub then recovers the remainder as dividend - q*divisor).

               Register roles:
                 x12 (SCRATCH_ARM) - base pointer, copied from rBUF up
                              front, same reasoning as x86-64's r14
                 x13          - the running write index during the
                              digit loop; once the loop ends it holds
                              the total byte count
                 x14          - magnitude/quotient/remainder work
                              during the digit loop; the swap's first
                              byte temp during the reversal
                 rBUF's own physical register - divisor-10 / quotient
                              scratch during the digit loop, then the
                              reversal's 'lo' index (safe to reuse,
                              rBUF is a documented write target here)
                 rLEN's own physical register - holds the total byte
                              count from just after the digit loop
                              onward; that's also literally the correct
                              final answer, so it's written once right
                              there and never touched again -- which
                              frees it up for nothing else, unlike
                              x86-64's version where the equivalent
                              register doubles as reversal scratch

               AArch64 only has x12/x13/x14 plus rBUF/rLEN's borrowed
               registers to work with -- one fewer truly free register
               than x86-64 has (no r15-equivalent). The reversal's
               byte-swap needs two live index registers and two live
               byte temps at once, which doesn't quite fit that budget
               alongside a persisted 'hi' index, so the total digit
               count is spilled to a small self-contained stack slot
               for the reversal's duration (opened and closed within
               this one instruction, symmetric, and touching nothing
               the caller's frame relies on) -- the same trade a
               register allocator would make under this kind of
               pressure. */
            const char *bufreg = reg__name(TARGET_AARCH64, &ins->dst);
            const char *lenreg = reg__name(TARGET_AARCH64, &ins->len_reg);
            char bufreg_w[8];
            snprintf(bufreg_w, sizeof(bufreg_w), "w%s", bufreg + 1);
            char lenreg_w[8];
            snprintf(lenreg_w, sizeof(lenreg_w), "w%s", lenreg + 1);
            const char *srcreg = reg__name(TARGET_AARCH64, &ins->src);

            char id[16];
            snprintf(id, sizeof(id), "%d", g_label_counter++);
            char pos_lbl[40], magready_lbl[40], loop_lbl2[40], digitsdone_lbl[40];
            char revbounds_lbl[40], revswap_lbl[40], revcheck_lbl[40];
            snprintf(pos_lbl, sizeof(pos_lbl), ".Li2s_pos%s", id);
            snprintf(magready_lbl, sizeof(magready_lbl), ".Li2s_magready%s", id);
            snprintf(loop_lbl2, sizeof(loop_lbl2), ".Li2s_loop%s", id);
            snprintf(digitsdone_lbl, sizeof(digitsdone_lbl), ".Li2s_digitsdone%s", id);
            snprintf(revbounds_lbl, sizeof(revbounds_lbl), ".Li2s_revbounds%s", id);
            snprintf(revswap_lbl, sizeof(revswap_lbl), ".Li2s_revswap%s", id);
            snprintf(revcheck_lbl, sizeof(revcheck_lbl), ".Li2s_revcheck%s", id);

            fprintf(out, "    mov %s, %s\n", SCRATCH_ARM, bufreg);
            fprintf(out, "    mov x14, %s\n", srcreg);
            fprintf(out, "    cmp x14, #0\n");
            fprintf(out, "    b.ge %s\n", pos_lbl);
            fprintf(out, "    mov %s, #45\n", bufreg_w); /* '-' */
            fprintf(out, "    strb %s, [%s]\n", bufreg_w, SCRATCH_ARM);
            fprintf(out, "    neg x14, x14\n");
            fprintf(out, "    mov x13, #1\n");
            fprintf(out, "    b %s\n", magready_lbl);
            fprintf(out, "%s:\n", pos_lbl);
            fprintf(out, "    mov x13, #0\n");
            fprintf(out, "%s:\n", magready_lbl);
            fprintf(out, "    cbnz x14, %s\n", loop_lbl2);
            fprintf(out, "    mov %s, #48\n", bufreg_w); /* '0' */
            fprintf(out, "    strb %s, [%s, x13]\n", bufreg_w, SCRATCH_ARM);
            fprintf(out, "    add x13, x13, #1\n");
            fprintf(out, "    b %s\n", digitsdone_lbl);
            fprintf(out, "%s:\n", loop_lbl2);
            fprintf(out, "    mov %s, #10\n", lenreg);
            fprintf(out, "    udiv %s, x14, %s\n", bufreg, lenreg);
            fprintf(out, "    msub x14, %s, %s, x14\n", bufreg, lenreg);
            fprintf(out, "    add x14, x14, #48\n");
            fprintf(out, "    strb w14, [%s, x13]\n", SCRATCH_ARM);
            fprintf(out, "    add x13, x13, #1\n");
            fprintf(out, "    mov x14, %s\n", bufreg);
            fprintf(out, "    cbnz x14, %s\n", loop_lbl2);
            fprintf(out, "%s:\n", digitsdone_lbl);
            /* Spill the total count so x13 is free to become 'hi'
               during the reversal below. */
            fprintf(out, "    sub sp, sp, #16\n");
            fprintf(out, "    str x13, [sp]\n");
            /* Peek-based lo: buf[0] is '-' iff the number was negative. */
            fprintf(out, "    mov %s, #0\n", bufreg);
            fprintf(out, "    ldrb w14, [%s]\n", SCRATCH_ARM);
            fprintf(out, "    cmp w14, #45\n");
            fprintf(out, "    b.ne %s\n", revbounds_lbl);
            fprintf(out, "    mov %s, #1\n", bufreg);
            fprintf(out, "%s:\n", revbounds_lbl);
            fprintf(out, "    ldr x13, [sp]\n");
            fprintf(out, "    sub x13, x13, #1\n");
            fprintf(out, "    b %s\n", revcheck_lbl);
            fprintf(out, "%s:\n", revswap_lbl);
            fprintf(out, "    ldrb w14, [%s, %s]\n", SCRATCH_ARM, bufreg);
            fprintf(out, "    ldrb %s, [%s, x13]\n", lenreg_w, SCRATCH_ARM);
            fprintf(out, "    strb %s, [%s, %s]\n", lenreg_w, SCRATCH_ARM, bufreg);
            fprintf(out, "    strb w14, [%s, x13]\n", SCRATCH_ARM);
            fprintf(out, "    add %s, %s, #1\n", bufreg, bufreg);
            fprintf(out, "    sub x13, x13, #1\n");
            fprintf(out, "%s:\n", revcheck_lbl);
            fprintf(out, "    cmp %s, x13\n", bufreg);
            fprintf(out, "    b.lt %s\n", revswap_lbl);
            fprintf(out, "    ldr %s, [sp]\n", lenreg);
            fprintf(out, "    add sp, sp, #16\n");
            break;
        }

        case OP_S2I: {
            /* s2i rBUF, rLEN > rDST. rBUF/rLEN are read-only inputs
               (see the pin-check comment on OP_S2I), so unlike i2s
               their physical registers stay untouched throughout --
               everything works out of x12/x13/x14, which is enough
               since there's no reversal to fund here, only a single
               forward accumulate pass.

               The accumulator runs NEGATIVE throughout (dst = dst*10 -
               digit), regardless of the source string's own sign, and
               is only negated back to positive at the very end if the
               string had no leading '-'. See the x86-64 OP_S2I case's
               comment for the full reasoning: accumulating positively
               and negating at the end overflows int64 by exactly one
               for "-9223372036854775808" (INT64_MIN), since its
               magnitude doesn't fit as a positive int64 value in the
               first place. Accumulating negatively sidesteps that
               entirely -- INT64_MIN fits directly with no negation
               needed, and the positive path's own largest possible
               magnitude (INT64_MAX) negates back cleanly. */
            const char *bufreg = reg__name(TARGET_AARCH64, &ins->dst);
            const char *lenreg = reg__name(TARGET_AARCH64, &ins->len_reg);
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->result_reg);

            char id[16];
            snprintf(id, sizeof(id), "%d", g_label_counter++);
            char loop_lbl2[40], negcheck_lbl[40], done_lbl2[40];
            snprintf(loop_lbl2, sizeof(loop_lbl2), ".Ls2i_loop%s", id);
            snprintf(negcheck_lbl, sizeof(negcheck_lbl), ".Ls2i_negcheck%s", id);
            snprintf(done_lbl2, sizeof(done_lbl2), ".Ls2i_done%s", id);

            fprintf(out, "    mov %s, #0\n", dstreg); /* accumulator must start at 0 -- a pre-existing gap independent of the INT64_MIN fix above, found while verifying it: dstreg otherwise carries in whatever it last held */
            fprintf(out, "    mov x12, #0\n");
            fprintf(out, "    cbz %s, %s\n", lenreg, done_lbl2);
            fprintf(out, "    ldrb w14, [%s]\n", bufreg);
            fprintf(out, "    cmp w14, #45\n");
            fprintf(out, "    b.ne %s\n", loop_lbl2);
            fprintf(out, "    mov x12, #1\n");
            fprintf(out, "%s:\n", loop_lbl2);
            fprintf(out, "    cmp x12, %s\n", lenreg);
            fprintf(out, "    b.ge %s\n", negcheck_lbl);
            fprintf(out, "    ldrb w13, [%s, x12]\n", bufreg);
            fprintf(out, "    sub x13, x13, #48\n");
            fprintf(out, "    mov x14, #10\n");
            fprintf(out, "    mul %s, %s, x14\n", dstreg, dstreg);
            fprintf(out, "    sub %s, %s, x13\n", dstreg, dstreg); /* accumulate negatively -- see comment above */
            fprintf(out, "    add x12, x12, #1\n");
            fprintf(out, "    b %s\n", loop_lbl2);
            fprintf(out, "%s:\n", negcheck_lbl);
            fprintf(out, "    ldrb w14, [%s]\n", bufreg);
            fprintf(out, "    cmp w14, #45\n");
            fprintf(out, "    b.eq %s\n", done_lbl2); /* already negative, and correctly so */
            fprintf(out, "    neg %s, %s\n", dstreg, dstreg); /* positive path: flip back */
            fprintf(out, "%s:\n", done_lbl2);
            break;
        }

        case OP_RAW:
            fprintf(out, "    %s\n", ins->raw_text);
            break;

        case OP_RAWDATA: {
            /* Same .byte/.hword/.word/.xword choice the SEC_DATA
               'is_data_array' loop above uses for 'data', dropped inline
               into .text instead. GAS is happy to mix data directives
               into .text (same reasoning as the x86-64 backend).
               For raw_data_is_float, dsize is only ever 4 or 8, so
               .word/.xword still cover it -- values go through
               float__bits/double__bits as hex, same as the SEC_DATA
               float-array loop above. */
            const char *dsz = ins->raw_data_size == 1 ? ".byte" :
                               ins->raw_data_size == 2 ? ".hword" :
                               ins->raw_data_size == 4 ? ".word" : ".xword";
            fprintf(out, "    %s ", dsz);
            for (int v = 0; v < ins->raw_data_nvals; v++) {
                if (ins->raw_data_is_float) {
                    if (ins->raw_data_size == 4)
                        fprintf(out, "%s0x%08x", v == 0 ? "" : ", ", float__bits(ins->raw_data_fvals[v]));
                    else
                        fprintf(out, "%s0x%016llx", v == 0 ? "" : ", ", (unsigned long long)double__bits(ins->raw_data_fvals[v]));
                } else {
                    /* '&label' entries -- see arm_emit_int_val's comment
                       and the 'bytes' parsing branch's own comment. */
                    arm_emit_int_val(out, ins->raw_data_vals[v], ins->raw_data_val_is_label, ins->raw_data_val_labels, v, v == 0);
                }
            }
            fprintf(out, "\n");
            break;
        }

        case OP_FENCE:
            /* dmb ish: full inner-shareable-domain data memory barrier --
               the standard "everyone in this coherence domain sees this
               order" fence on AArch64, matching mfence's role on x86-64.
               ish (not sy) since Chard's target is ordinary application
               code within one coherence domain, not cross-domain device
               memory. Directional variants for the weaker orderings:
               ishld orders loads-before-loads/stores only (acquire),
               ishst orders stores-before-stores only (release); plain
               ish (both directions) covers seq_cst and acq_rel, which
               collapse to the same barrier here for the same reason
               they collapse to the same ldaxr/stlxr pair in the RMW
               ops above. A relaxed fence establishes no ordering by
               definition -- emitted as a comment rather than silently
               nothing, so the output still shows where the (no-op)
               fence was requested, matching this project's convention
               of never silently dropping something the source asked
               for (see e.g. the OP_ASSERT/exit(N) discussion elsewhere
               in this file). */
            if (ins->mem_order == MEM_ORDER_RELAXED)
                fprintf(out, "    // fence %%relaxed -- no ordering requested, no instruction needed\n");
            else if (ins->mem_order == MEM_ORDER_ACQUIRE)
                fprintf(out, "    dmb ishld\n");
            else if (ins->mem_order == MEM_ORDER_RELEASE)
                fprintf(out, "    dmb ishst\n");
            else
                fprintf(out, "    dmb ish\n");
            break;

        /* Floats. f1-f7 map to d0-d6 (the 64-bit view of AArch64's
           v0-v6 FP/SIMD registers), always holding a double -- same
           "always compute at f64" model as x86-64 (see the OP_FADD
           family comment in the opcode_t enum). d7 is fscratch. AArch64
           has no direct float-immediate-load instruction for arbitrary
           doubles either, so a literal is materialized the same way as
           x86-64: build its bit pattern in an integer scratch register
           (via a sequence of movz/movk, since AArch64's mov/movz can
           only load 16 bits at a time), then fmov it into the
           destination d register. */
        case OP_FMOV:
            /* fmov's mnemonic doesn't change between s/d forms -- only
               the register name does, and reg__name already picks
               sregs vs fregs via is_f32. Only the immediate path needs
               narrowing: an f32 dst materializes a 32-bit bit pattern
               (movz+movk once, into the scratch register's w-view)
               instead of the full 64-bit movz+movk*3 sequence. */
            if (ins->src.kind == OPND_IMM) {
                if (ins->dst.is_f32) {
                    /* SCRATCH_ARM is "x12"; its w-view is "w12" -- skip
                       the leading 'x' rather than hardcoding the number,
                       so this keeps working if scratch ever changes. */
                    uint32_t bits = float__bits(ins->src.fimm);
                    fprintf(out, "    movz w%s, #0x%04x\n", SCRATCH_ARM + 1, (unsigned)(bits & 0xffff));
                    fprintf(out, "    movk w%s, #0x%04x, lsl #16\n", SCRATCH_ARM + 1, (unsigned)((bits >> 16) & 0xffff));
                    fprintf(out, "    fmov %s, w%s\n", reg__name(TARGET_AARCH64, &ins->dst), SCRATCH_ARM + 1);
                } else {
                    uint64_t bits = double__bits(ins->src.fimm);
                    fprintf(out, "    movz %s, #0x%04x\n", SCRATCH_ARM, (unsigned)(bits & 0xffff));
                    fprintf(out, "    movk %s, #0x%04x, lsl #16\n", SCRATCH_ARM, (unsigned)((bits >> 16) & 0xffff));
                    fprintf(out, "    movk %s, #0x%04x, lsl #32\n", SCRATCH_ARM, (unsigned)((bits >> 32) & 0xffff));
                    fprintf(out, "    movk %s, #0x%04x, lsl #48\n", SCRATCH_ARM, (unsigned)((bits >> 48) & 0xffff));
                    fprintf(out, "    fmov %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst), SCRATCH_ARM);
                }
            } else {
                fprintf(out, "    fmov %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst), reg__name(TARGET_AARCH64, &ins->src));
            }
            break;

        case OP_FLOAD: {
            /* dst.is_f32 now decides the load width directly --
               reg__name already returns the correct-width register
               name (s0-s6 vs d0-d6) for the destination, so this is a
               plain ldr with no fcvt/scratch-routing step at all,
               unlike the old always-widen-to-f64 behavior. */
            const char *addrreg = emit_aarch64_addr_into_scratch(out, &ins->src);
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            fprintf(out, "    ldr %s, [%s]\n", dstreg, addrreg);
            break;
        }

        case OP_FSTORE: {
            /* Mirrors OP_FLOAD: src.is_f32 already picks the correct
               register name via reg__name, so a plain str stores the
               right width with no fcvt step. */
            const char *addrreg = emit_aarch64_addr_into_scratch(out, &ins->dst);
            const char *srcreg = reg__name(TARGET_AARCH64, &ins->src);
            fprintf(out, "    str %s, [%s]\n", srcreg, addrreg);
            break;
        }

        case OP_VLOAD: {
            /* ldr qN, [addr] -- 128-bit NEON load, the vector-register
               sibling of OP_FLOAD's scalar 'ldr dN, [addr]'. Same
               'dN' -> 'qN' leading-letter swap already used to spell
               vN.2d for the arithmetic vN family (see OP_VADD's
               AArch64 case) -- reg__name gives the scalar spelling,
               renamed here rather than looked up in a separate table,
               since Chard has no vector register class of its own.
               Unlike x86-64's movupd/movapd split, AArch64's ldr with
               a q-register does not require 16-byte alignment by
               default, so no unaligned-vs-aligned choice is needed
               here even though a two-element f64 array's base is only
               guaranteed 8-byte aligned (see OP_VLOAD's opcode_t
               comment) -- ldr q just works either way. */
            const char *addrreg = emit_aarch64_addr_into_scratch(out, &ins->src);
            const char *dstd = reg__name(TARGET_AARCH64, &ins->dst);
            fprintf(out, "    ldr q%s, [%s]\n", dstd + 1, addrreg);
            break;
        }

        case OP_VSTORE: {
            /* Mirrors OP_VLOAD: str qN, [addr], same alignment-free
               reasoning. */
            const char *addrreg = emit_aarch64_addr_into_scratch(out, &ins->dst);
            const char *srcd = reg__name(TARGET_AARCH64, &ins->src);
            fprintf(out, "    str q%s, [%s]\n", srcd + 1, addrreg);
            break;
        }

        case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
            /* Mnemonic doesn't change between s/d forms (see FMOV's
               comment) -- reg__name already picks the right-width
               register name for dstreg via dst.is_f32. Only the
               immediate materialization needs narrowing, same pattern
               as FMOV's immediate path. */
            const char *mn = ins->op == OP_FADD ? "fadd" : ins->op == OP_FSUB ? "fsub" :
                              ins->op == OP_FMUL ? "fmul" : "fdiv";
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            if (ins->src.kind == OPND_IMM) {
                if (ins->dst.is_f32) {
                    uint32_t bits = float__bits(ins->src.fimm);
                    fprintf(out, "    movz w%s, #0x%04x\n", SCRATCH_ARM + 1, (unsigned)(bits & 0xffff));
                    fprintf(out, "    movk w%s, #0x%04x, lsl #16\n", SCRATCH_ARM + 1, (unsigned)((bits >> 16) & 0xffff));
                    fprintf(out, "    fmov %s, w%s\n", target_defs[TARGET_AARCH64].sscratch, SCRATCH_ARM + 1);
                    fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, target_defs[TARGET_AARCH64].sscratch);
                } else {
                    uint64_t bits = double__bits(ins->src.fimm);
                    fprintf(out, "    movz %s, #0x%04x\n", SCRATCH_ARM, (unsigned)(bits & 0xffff));
                    fprintf(out, "    movk %s, #0x%04x, lsl #16\n", SCRATCH_ARM, (unsigned)((bits >> 16) & 0xffff));
                    fprintf(out, "    movk %s, #0x%04x, lsl #32\n", SCRATCH_ARM, (unsigned)((bits >> 32) & 0xffff));
                    fprintf(out, "    movk %s, #0x%04x, lsl #48\n", SCRATCH_ARM, (unsigned)((bits >> 48) & 0xffff));
                    fprintf(out, "    fmov %s, %s\n", target_defs[TARGET_AARCH64].fscratch, SCRATCH_ARM);
                    fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, target_defs[TARGET_AARCH64].fscratch);
                }
            } else {
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, reg__name(TARGET_AARCH64, &ins->src));
            }
            break;
        }

        case OP_VADD: case OP_VSUB: case OP_VMUL: case OP_VDIV:
        case OP_VMIN: case OP_VMAX: {
            /* NEON fadd/fsub/fmul/fdiv/fmin/fmax v.2d, v.2d, v.2d --
               packed-double arithmetic over the same 128-bit register
               OP_FADD's scalar 'dN' form already names the low half of
               (AArch64's d0-d31 and v0-v31 are the same physical
               registers under different views, same relationship as
               x86-64's xmm names for OP_FADD vs OP_VADD/etc above).
               reg__name gives the scalar 'dN' spelling every other
               float op uses; renamed here to the vector 'vN.2d'
               spelling by swapping the leading letter, since there's no
               separate vector-register table to reuse (see the OP_VADD
               opcode_t comment -- Chard has no vector register class of
               its own, this just reinterprets the existing f-register
               file). No immediate-source path -- parser already
               requires a register src for all six of these. NEON's
               vector fmin/fmax follow the same "NaN loses to a real
               operand" convention as their scalar forms (see OP_VMIN's
               opcode_t comment), so no extra handling is needed here
               either. */
            const char *mn = ins->op == OP_VADD ? "fadd" : ins->op == OP_VSUB ? "fsub" :
                              ins->op == OP_VMUL ? "fmul" : ins->op == OP_VDIV ? "fdiv" :
                              ins->op == OP_VMIN ? "fmin" : "fmax";
            const char *dstd = reg__name(TARGET_AARCH64, &ins->dst);
            const char *srcd = reg__name(TARGET_AARCH64, &ins->src);
            fprintf(out, "    %s v%s.2d, v%s.2d, v%s.2d\n", mn, dstd + 1, dstd + 1, srcd + 1);
            break;
        }

        case OP_VSQRT: case OP_VABS: case OP_VNEG: {
            /* NEON fsqrt/fabs/fneg v.2d, v.2d -- unary packed-double
               instructions, same 'vN.2d' renaming trick as the
               destructive vN family just above (see that case's
               comment for why reg__name's scalar 'dN' spelling gets
               its leading letter swapped rather than looked up in a
               separate table). Each has a dedicated native vector
               form, same as their scalar dN counterparts (OP_FSQRT/
               OP_FABS/OP_FNEG) already do -- no mask dance needed
               here any more than it is there, unlike x86-64's
               andpd/xorpd idiom for vabs/vneg. No immediate-source
               path -- parser already requires a register src (see the
               OP_VSQRT/OP_VABS/OP_VNEG parse-time case). */
            const char *mn = ins->op == OP_VSQRT ? "fsqrt" : ins->op == OP_VABS ? "fabs" : "fneg";
            const char *dstd = reg__name(TARGET_AARCH64, &ins->dst);
            const char *srcd = reg__name(TARGET_AARCH64, &ins->src);
            fprintf(out, "    %s v%s.2d, v%s.2d\n", mn, dstd + 1, srcd + 1);
            break;
        }

        case OP_VDUP: {
            /* NEON dup vDST.2d, vSRC.d[0] -- dedicated broadcast-from-
               lane-0 instruction: reads src's low (and only relevant)
               64-bit lane and replicates it into both lanes of dst.
               Unlike OP_VSQRT/OP_VABS/OP_VNEG above this isn't a
               'vN.2d, vN.2d' whole-register form -- the source operand
               is written 'vSRC.d[0]' (an explicit single-lane index)
               rather than 'vSRC.2d', since dup's source is always one
               scalar lane, never a whole packed register, regardless
               of what dst ends up as. Same dN-to-vN leading-letter-swap
               trick as its siblings for both operands. No mask, no
               scratch register, no immediate-source path (register-
               only, see the OP_VDUP parse-time case, which folds into
               the same case as vsqrt/vabs/vneg). */
            const char *dstd = reg__name(TARGET_AARCH64, &ins->dst);
            const char *srcd = reg__name(TARGET_AARCH64, &ins->src);
            fprintf(out, "    dup v%s.2d, v%s.d[0]\n", dstd + 1, srcd + 1);
            break;
        }

        case OP_VFMA: {
            /* fDST = fA*fB + fC lane-wise. NEON's fmla is accumulating
               (dst += fA*fB), not a fresh three-source op the way
               scalar fmadd is (OP_FMA's own AArch64 case), so fC is
               moved into dst first via a whole-register fmov (16-byte
               'v.16b' form -- fmov doesn't have a '.2d' spelling the
               way the arithmetic ops do, since it's a plain bit copy,
               not a lane-wise float op), then fmla v.2d accumulates
               fA*fB on top. Same dN-to-vN leading-letter-swap trick as
               OP_VDUP/etc for every operand. fA/fB/fC are
               cas_expected/result_reg/cas_desired respectively, same
               reuse as OP_FMA/OP_VFMA elsewhere. The move is skipped
               when fC and dst are already the same register, same
               move-if-different guard OP_FMA's own case uses. */
            const char *dstd = reg__name(TARGET_AARCH64, &ins->dst);
            const char *ad = reg__name(TARGET_AARCH64, &ins->cas_expected);
            const char *bd = reg__name(TARGET_AARCH64, &ins->result_reg);
            const char *cd = reg__name(TARGET_AARCH64, &ins->cas_desired);
            if (strcmp(dstd, cd) != 0)
                fprintf(out, "    fmov v%s.16b, v%s.16b\n", dstd + 1, cd + 1);
            fprintf(out, "    fmla v%s.2d, v%s.2d, v%s.2d\n", dstd + 1, ad + 1, bd + 1);
            break;
        }

        case OP_FCMP:
            /* fcmp dst, src -- same LHS/RHS convention as OP_CMP (see
               OP_CMP's own comment at parse time and the opcode_t
               comment on OP_FCMP). AArch64's fcmp sets NZCV directly
               from an IEEE-754 comparison, including the "unordered"
               (NaN) case setting C=1,V=1,Z=0,N=0 -- which the existing
               unsigned hi/lo condition codes (used by Chard's ja/jb/jae/
               jbe) already read as "greater", matching the same
               "unordered compares as greater" convention x86-64's
               ucomisd gives for free. */
            fprintf(out, "    fcmp %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst), reg__name(TARGET_AARCH64, &ins->src));
            break;

        case OP_FSQRT:
            /* fsqrt: direct native instruction, no immediate case to
               special-case since this is unary (src is the whole
               input -- see the parse-time comment). Negative input
               naturally produces NaN, matching the opcode_t comment's
               contract. */
            fprintf(out, "    fsqrt %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst), reg__name(TARGET_AARCH64, &ins->src));
            break;

        case OP_FABS:
            /* fabs: direct native instruction (clears the sign bit in
               hardware) -- unlike x86-64's SSE2, AArch64's FP/SIMD unit
               has a dedicated scalar fabs, so no mask-and-andpd dance
               is needed here. */
            fprintf(out, "    fabs %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst), reg__name(TARGET_AARCH64, &ins->src));
            break;

        case OP_FNEG:
            /* fneg: direct native instruction (flips the sign bit in
               hardware) -- same "no mask dance needed" situation as
               fabs just above, since AArch64's FP/SIMD unit has a
               dedicated scalar fneg too. */
            fprintf(out, "    fneg %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst), reg__name(TARGET_AARCH64, &ins->src));
            break;

        case OP_FMIN: case OP_FMAX: {
            /* fmin/fmax: direct native instructions, same destructive
               dst-OP-src shape and immediate-materialization path as
               fadd/etc above (mirrored identically -- see that case's
               comment). Both already implement the "NaN loses to a
               real operand" convention documented on the OP_FMIN/
               OP_FMAX opcode_t comment. Mnemonic doesn't change between
               s/d forms -- reg__name already picks the right-width
               register name for dstreg via dst.is_f32 -- only the
               immediate materialization needs narrowing, same pattern
               as FADD's immediate path. */
            const char *mn = ins->op == OP_FMIN ? "fmin" : "fmax";
            const char *dstreg = reg__name(TARGET_AARCH64, &ins->dst);
            if (ins->src.kind == OPND_IMM) {
                if (ins->dst.is_f32) {
                    uint32_t bits = float__bits(ins->src.fimm);
                    fprintf(out, "    movz w%s, #0x%04x\n", SCRATCH_ARM + 1, (unsigned)(bits & 0xffff));
                    fprintf(out, "    movk w%s, #0x%04x, lsl #16\n", SCRATCH_ARM + 1, (unsigned)((bits >> 16) & 0xffff));
                    fprintf(out, "    fmov %s, w%s\n", target_defs[TARGET_AARCH64].sscratch, SCRATCH_ARM + 1);
                    fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, target_defs[TARGET_AARCH64].sscratch);
                } else {
                    uint64_t bits = double__bits(ins->src.fimm);
                    fprintf(out, "    movz %s, #0x%04x\n", SCRATCH_ARM, (unsigned)(bits & 0xffff));
                    fprintf(out, "    movk %s, #0x%04x, lsl #16\n", SCRATCH_ARM, (unsigned)((bits >> 16) & 0xffff));
                    fprintf(out, "    movk %s, #0x%04x, lsl #32\n", SCRATCH_ARM, (unsigned)((bits >> 32) & 0xffff));
                    fprintf(out, "    movk %s, #0x%04x, lsl #48\n", SCRATCH_ARM, (unsigned)((bits >> 48) & 0xffff));
                    fprintf(out, "    fmov %s, %s\n", target_defs[TARGET_AARCH64].fscratch, SCRATCH_ARM);
                    fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, target_defs[TARGET_AARCH64].fscratch);
                }
            } else {
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, reg__name(TARGET_AARCH64, &ins->src));
            }
            break;
        }

        case OP_FMA:
            /* fmadd dDST, fA, fB, fC computes dDST = fA*fB + fC directly
               (AArch64's fused multiply-add takes all three source
               registers as distinct operands, unlike x86-64's
               destructive 213-form -- no pre-move into dst needed
               here). fA/fB/fC are cas_expected/result_reg/cas_desired
               respectively -- see the instr_t field comments and the
               OP_FMA opcode_t comment for why those fields are reused. */
            fprintf(out, "    fmadd %s, %s, %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst),
                    reg__name(TARGET_AARCH64, &ins->cas_expected), reg__name(TARGET_AARCH64, &ins->result_reg),
                    reg__name(TARGET_AARCH64, &ins->cas_desired));
            break;

        case OP_I2F:
            if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    mov %s, #%ld\n", SCRATCH_ARM, ins->src.imm);
                fprintf(out, "    scvtf %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst), SCRATCH_ARM);
            } else {
                fprintf(out, "    scvtf %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst), reg__name(TARGET_AARCH64, &ins->src));
            }
            break;

        case OP_F2I:
            /* fcvtzs: convert-to-signed-integer, round toward zero --
               matching the opcode_t comment's "truncating toward zero"
               contract (plain fcvtas/fcvtns would round instead). */
            fprintf(out, "    fcvtzs %s, %s\n", reg__name(TARGET_AARCH64, &ins->dst), reg__name(TARGET_AARCH64, &ins->src));
            break;

        case OP_SYSCALL: {
            /* Linux AArch64 syscall ABI: number in x8, args in x0-x5.
               Only the args the user actually wrote are emitted. */
            static const char *argregs[6] = {"x0", "x1", "x2", "x3", "x4", "x5"};
            for (int a = 1; a < ins->nargs; a++) {
                /* '&SYM' (is_addr_of): materialize SYM's address directly
                   into the arg register, same adrp+add pair OP_LEA uses,
                   instead of the value-load emit_aarch64_load_scratch
                   would otherwise perform. Must be checked before the
                   is_mem_operand branch below -- see the matching x86-64
                   comment for why. check_addr_of_violations() guarantees
                   is_addr_of can't reach here except in this exact slot. */
                if (ins->args[a].kind == OPND_SYM && ins->args[a].is_addr_of) {
                    const char *r = argregs[a - 1];
                    fprintf(out, "    adrp %s, %s\n", r, ins->args[a].sym);
                    fprintf(out, "    add %s, %s, :lo12:%s\n", r, r, ins->args[a].sym);
                    continue;
                }
                if (ins->args[a].kind == OPND_LOCAL && ins->args[a].is_addr_of) {
                    /* '&local' (is_addr_of on OPND_LOCAL): same idea as
                       the OPND_SYM branch above, just fp-relative
                       (x29-based) instead of adrp/add. aarch64_local_base
                       already emits the frames_up saved-x29 chase (if
                       any) and returns the register to subtract
                       local_offset from -- exactly the computation
                       emit_aarch64_addr_into_scratch performs for
                       atomics, just landed directly in this arg's slot
                       register instead of always SCRATCH_ARM. */
                    const char *r = argregs[a - 1];
                    const char *base = aarch64_local_base(out, &ins->args[a]);
                    aarch64_emit_local_addr(out, r, base, ins->args[a].local_offset);
                    continue;
                }
                if (is_mem_operand(ins->args[a].kind)) {
                    emit_aarch64_load_scratch(out, &ins->args[a]);
                    fprintf(out, "    mov %s, %s\n", argregs[a - 1], SCRATCH_ARM);
                } else {
                    render_simple_operand(TARGET_AARCH64, &ins->args[a], sb, sizeof(sb));
                    fprintf(out, "    mov %s, %s\n", argregs[a - 1], sb);
                }
            }
            if (is_mem_operand(ins->args[0].kind)) {
                emit_aarch64_load_scratch(out, &ins->args[0]);
                fprintf(out, "    mov x8, %s\n", SCRATCH_ARM);
            } else {
                render_simple_operand(TARGET_AARCH64, &ins->args[0], sb, sizeof(sb));
                fprintf(out, "    mov x8, %s\n", sb);
            }
            fprintf(out, "    svc #0\n");
            break;
        }

        case OP_READ: case OP_WRITE: {
            /* Named wrappers around read(2)/write(2): same 3-register
               argument shape as OP_SYSCALL (fd, buf, len -> x0, x1,
               x2), just with the syscall number already chosen instead
               of taken from args[0]. Linux AArch64: read = 63,
               write = 64 (matching the number OP_STDOUT already uses).

               buf (arg 1) is always treated as an address, same
               reasoning as the x86-64 case above: a symbol/local names
               the buffer, so it needs its address computed (mirroring
               OP_LEA's own local-vs-symbol branching below), not a
               value load. A register operand passes through unchanged. */
            static const char *argregs3[3] = {"x0", "x1", "x2"};
            for (int a = 0; a < 3; a++) {
                if (a == 1 && ins->args[a].kind == OPND_LOCAL) {
                    const char *base = aarch64_local_base(out, &ins->args[a]);
                    aarch64_emit_local_addr(out, argregs3[a], base, ins->args[a].local_offset);
                    continue;
                }
                if (a == 1 && ins->args[a].kind == OPND_SYM) {
                    fprintf(out, "    adrp %s, %s\n", argregs3[a], ins->args[a].sym);
                    fprintf(out, "    add %s, %s, :lo12:%s\n", argregs3[a], argregs3[a], ins->args[a].sym);
                    continue;
                }
                if (is_mem_operand(ins->args[a].kind)) {
                    emit_aarch64_load_scratch(out, &ins->args[a]);
                    fprintf(out, "    mov %s, %s\n", argregs3[a], SCRATCH_ARM);
                } else {
                    render_simple_operand(TARGET_AARCH64, &ins->args[a], sb, sizeof(sb));
                    fprintf(out, "    mov %s, %s\n", argregs3[a], sb);
                }
            }
            fprintf(out, "    mov x8, #%d\n", ins->op == OP_READ ? 63 : 64);
            fprintf(out, "    svc #0\n");
            break;
        }

        case OP_LIBC_INIT:
            /* No code needed: AAPCS64/glibc's own crt startup already
               ran before 'main' (the retargeted entry label -- see
               apply_entry_symbol_override) is reached. Same story as the
               x86-64 case above. */
            break;

        case OP_LIBC_CALL: {
            /* AAPCS64: integer/pointer args in x0-x7 in order; Chard
               caps libcall at MAX_LIBC_ARGS (6), so only x0-x5 are ever
               used here. No vector-argument-count register to set
               (unlike x86-64's al) -- AAPCS64 has no such requirement
               for variadic calls. Stack alignment: sp must be 16-byte
               aligned at 'bl', which holds for the same reasons noted
               in the x86-64 OP_LIBC_CALL comment (main is entered
               aligned; every Chard sp-touching construct keeps pairs
               aligned). */
            static const char *libc_argregs[MAX_LIBC_ARGS] = {"x0", "x1", "x2", "x3", "x4", "x5"};
            for (int a = 0; a < ins->nargs; a++) {
                if (ins->args[a].kind == OPND_SYM && ins->args[a].is_addr_of) {
                    const char *r = libc_argregs[a];
                    fprintf(out, "    adrp %s, %s\n", r, ins->args[a].sym);
                    fprintf(out, "    add %s, %s, :lo12:%s\n", r, r, ins->args[a].sym);
                    continue;
                }
                if (ins->args[a].kind == OPND_LOCAL && ins->args[a].is_addr_of) {
                    const char *r = libc_argregs[a];
                    const char *base = aarch64_local_base(out, &ins->args[a]);
                    aarch64_emit_local_addr(out, r, base, ins->args[a].local_offset);
                    continue;
                }
                if (is_mem_operand(ins->args[a].kind)) {
                    emit_aarch64_load_scratch(out, &ins->args[a]);
                    fprintf(out, "    mov %s, %s\n", libc_argregs[a], SCRATCH_ARM);
                } else {
                    render_simple_operand(TARGET_AARCH64, &ins->args[a], sb, sizeof(sb));
                    fprintf(out, "    mov %s, %s\n", libc_argregs[a], sb);
                }
            }
            fprintf(out, "    bl %s\n", ins->dst.sym);
            if (ins->dst.kind == OPND_REG) {
                /* Return value comes back in x0; move it into the
                   requested r1-r12 slot unless it's already x0
                   (reg_num 1 -- see target_defs[TARGET_AARCH64].regs). */
                if (ins->dst.reg_num != 1)
                    fprintf(out, "    mov %s, x0\n", target_defs[TARGET_AARCH64].regs[ins->dst.reg_num]);
            }
            break;
        }
        default:
            /* See the matching x86-64 default: case for why this
               exists -- same safety net, same reasoning. */
            g_source_line = NULL;
            fail_fmt("internal error: chard AArch64 backend: unhandled opcode %d", (int)ins->op);
        }
    }
}

