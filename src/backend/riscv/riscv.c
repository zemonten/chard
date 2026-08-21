#include "../../chard.h"

const char *riscv_local_base(FILE *out, const operand_t *o, const char *scratch) {
    if (o->frames_up == 0) return "s0";
    fprintf(out, "    ld %s, 0(s0)\n", scratch);
    for (int i = 1; i < o->frames_up; i++)
        fprintf(out, "    ld %s, 0(%s)\n", scratch, scratch);
    return scratch;
}

const char *riscv_safe_offset(FILE *out, const char *base, long offset,
                                      const char *scratch, long *out_offset) {
    if (offset >= -2048 && offset <= 2047) {
        *out_offset = offset;
        return base;
    }
    /* addi's immediate is a 12-bit signed field, so an offset outside
       that range is materialized into `scratch` via the standard RISC-V
       lui+addi constant-building idiom (split into a 20-bit upper part
       and a 12-bit signed lower part, sign-adjusted) rather than a
       naive walk of <=2047-sized addi steps -- a 1 MiB local's worst-
       case offset would otherwise take 500+ chunked addi's to reach,
       which is correct but absurd codegen for something that fits in
       2-3 instructions. `scratch` is always distinct from `base` at
       every call site (a dedicated t0/t1/t2-class register, never the
       base itself), so it's safe to use as both the lui target and the
       final accumulator here. */
    long lo = offset & 0xFFF;
    if (lo >= 0x800) lo -= 0x1000;
    long hi = (offset - lo) >> 12;
    fprintf(out, "    lui %s, %ld\n", scratch, hi & 0xFFFFF);
    fprintf(out, "    add %s, %s, %s\n", scratch, base, scratch);
    if (lo != 0) fprintf(out, "    addi %s, %s, %ld\n", scratch, scratch, lo);
    *out_offset = 0;
    return scratch;
}

void emit_riscv_load_scratch(FILE *out, const operand_t *o, const char *scratch) {
    int sz = operand_mem_size(o);
    const char *mn = sz == 1 ? "lb" : sz == 2 ? "lh" : sz == 4 ? "lw" : "ld";
    if (o->kind == OPND_LOCAL) {
        /* RISC-V's load instructions already take an 'offset(reg)' form
           directly, so a local needs no address computation at all --
           unlike a linker symbol, which always goes through 'la' first.
           Offset/size were captured on the operand at parse time (see
           parse__operand), not re-looked-up here, since a local's
           decls[] entry no longer exists by the time codegen runs.
           Addressing via s0 (RISC-V's ABI-conventional frame-pointer
           register, aliased 'fp'), established once per @label block at
           OP_FRAME_OPEN and restored at OP_FRAME_CLOSE, rather than sp
           directly, is what keeps these offsets correct across nested
           blocks and any push/pop within a block that also has
           locals -- see the Locals section up top. A reference to a
           local declared in an enclosing block (frames_up > 0) chases
           the saved-fp chain first via riscv_local_base, reusing this
           same `scratch` register for both the chase and the final
           load -- safe since the chase completes (and is read) before
           the load instruction overwrites it. */
        const char *base = riscv_local_base(out, o, scratch);
        long safe_off;
        /* riscv_safe_offset may need to materialize into a scratch
           register too; reuse the same `scratch` the caller gave us --
           safe for the same reason the fp chase above can reuse it
           (nothing else is live in either register at this point). */
        base = riscv_safe_offset(out, base, -(long)o->local_offset, scratch, &safe_off);
        fprintf(out, "    %s %s, %ld(%s)\n", mn, scratch, safe_off, base);
        return;
    }
    if (o->kind == OPND_ADDR) {
        /* Absolute address -- RISC-V's 'li' pseudo-op (GAS-synthesized
           lui+addi, same idea as AArch64's 'mov' handling arbitrary
           64-bit constants) materializes it directly; no 'la'
           (link-time symbol address) applies here since there's no
           symbol, just a raw constant. */
        fprintf(out, "    li %s, %ld\n", scratch, o->imm);
        fprintf(out, "    %s %s, 0(%s)\n", mn, scratch, scratch);
        return;
    }
    fprintf(out, "    la %s, %s\n", scratch, o->sym);
    fprintf(out, "    %s %s, 0(%s)\n", mn, scratch, scratch);
}

void riscv_emit_local_addr(FILE *out, const char *dst, const char *base, long magnitude) {
    if (magnitude >= -2048 && magnitude <= 2047) {
        if (strcmp(dst, base) != 0) fprintf(out, "    mv %s, %s\n", dst, base);
        if (magnitude != 0) fprintf(out, "    addi %s, %s, %ld\n", dst, dst, magnitude);
        return;
    }
    long lo = magnitude & 0xFFF;
    if (lo >= 0x800) lo -= 0x1000;
    long hi = (magnitude - lo) >> 12;
    fprintf(out, "    lui %s, %ld\n", dst, hi & 0xFFFFF);
    fprintf(out, "    add %s, %s, %s\n", dst, base, dst);
    if (lo != 0) fprintf(out, "    addi %s, %s, %ld\n", dst, dst, lo);
}

void emit_riscv_mov_operand(FILE *out, const char *dstreg, const operand_t *o) {
    char sb[64];
    render_simple_operand(TARGET_RISCV, (operand_t *)o, sb, sizeof(sb));
    const char *mn = (o->kind == OPND_REG) ? "mv" : "li";
    fprintf(out, "    %s %s, %s\n", mn, dstreg, sb);
}

void riscv_emit_int_val(FILE *out, long val, const int *is_label, char *const *val_labels, int v, int first) {
    fprintf(out, "%s", first ? "" : ", ");
    if (is_label && is_label[v]) fprintf(out, "%s", val_labels[v]);
    else fprintf(out, "%ld", val);
}

void emit__riscv(FILE *out) {
    fprintf(out, "# Generated by Chard - target: RISC-V (RV64I%s)\n",
            g_uses_float ? "FD" : "");
    fprintf(out, "# GAS syntax\n");
    fprintf(out, g_mode == MODE_BARE
        ? "# mode: bare (raw/freestanding -- no ELF linkage scaffolding emitted)\n\n"
        : "# mode: elf (Linux)\n\n");
    if (g_uses_float) fprintf(out, "    .attribute arch, \"rv64i2p1_f2p2_d2p2\"\n\n");

    fprintf(out, ".data\n");
    for (int i = 0; i < ndecls; i++) {
        if (decls[i].section == SEC_DATA) {
            if (decls[i].is_data_array) {
                const char *dsz = decls[i].size_bytes == 1 ? ".byte" :
                                   decls[i].size_bytes == 2 ? ".half" :
                                   decls[i].size_bytes == 4 ? ".word" : ".dword";
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
                        riscv_emit_int_val(out, decls[i].data_vals[v], decls[i].data_val_is_label, decls[i].data_val_labels, v, v == 0);
                }
                fprintf(out, "\n");
            } else if (decls[i].is_ascii) {
                fprintf(out, "%s:\n    .byte ", decls[i].name);
                for (int b = 0; b < decls[i].str_len; b++) {
                    fprintf(out, "%s%d", b == 0 ? "" : ", ", (unsigned char)decls[i].str_val[b]);
                }
                if (decls[i].str_len == 0) fprintf(out, "0");
                fprintf(out, "\n%s_len:\n    .dword %d\n", decls[i].name, decls[i].str_len);
            } else if (decls[i].is_float) {
                if (decls[i].size_bytes == 4)
                    fprintf(out, "%s:\n    .word 0x%08x\n", decls[i].name, float__bits(decls[i].init_fvalue));
                else
                    fprintf(out, "%s:\n    .dword 0x%016llx\n", decls[i].name, (unsigned long long)double__bits(decls[i].init_fvalue));
            } else {
                const char *dsz = decls[i].size_bytes == 1 ? ".byte" :
                                   decls[i].size_bytes == 2 ? ".half" :
                                   decls[i].size_bytes == 4 ? ".word" : ".dword";
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
                                   decls[i].size_bytes == 2 ? ".half" :
                                   decls[i].size_bytes == 4 ? ".word" : ".dword";
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
                        riscv_emit_int_val(out, decls[i].data_vals[v], decls[i].data_val_is_label, decls[i].data_val_labels, v, v == 0);
                }
                fprintf(out, "\n");
            } else if (decls[i].is_ascii) {
                fprintf(out, "%s:\n    .byte ", decls[i].name);
                for (int b = 0; b < decls[i].str_len; b++) {
                    fprintf(out, "%s%d", b == 0 ? "" : ", ", (unsigned char)decls[i].str_val[b]);
                }
                if (decls[i].str_len == 0) fprintf(out, "0");
                fprintf(out, "\n%s_len:\n    .dword %d\n", decls[i].name, decls[i].str_len);
            } else if (decls[i].is_float) {
                if (decls[i].size_bytes == 4)
                    fprintf(out, "%s:\n    .word 0x%08x\n", decls[i].name, float__bits(decls[i].init_fvalue));
                else
                    fprintf(out, "%s:\n    .dword 0x%016llx\n", decls[i].name, (unsigned long long)double__bits(decls[i].init_fvalue));
            } else {
                const char *dsz = decls[i].size_bytes == 1 ? ".byte" :
                                   decls[i].size_bytes == 2 ? ".half" :
                                   decls[i].size_bytes == 4 ? ".word" : ".dword";
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

        switch (ins->op) {
        case OP_LABEL:
            fprintf(out, "%s:\n", ins->dst.sym);
            if (ins->is_entry && g_argv_seen) {
                /* '%argv rN, rM;' capture -- see the matching x86-64
                   OP_LABEL case for the full rationale; same ordering
                   requirement (before OP_FRAME_OPEN moves sp, before
                   anything clobbers a0/a1). */
                const char *argc_r = target_defs[TARGET_RISCV].regs[g_argv_argc_reg];
                const char *argv_r = target_defs[TARGET_RISCV].regs[g_argv_argv_reg];
                if (g_libc_linked) {
                    /* main(int argc, char **argv): argc in a0, argv in
                       a1, per the standard RISC-V calling convention. */
                    fprintf(out, "    mv %s, a0\n", argc_r);
                    fprintf(out, "    mv %s, a1\n", argv_r);
                } else {
                    /* Freestanding _start: [sp] = argc, [sp+8] = argv
                       (i.e. sp+8 is argv itself), same layout as the
                       other two targets. Must be read before
                       OP_FRAME_OPEN adjusts sp. */
                    fprintf(out, "    ld %s, 0(sp)\n", argc_r);
                    fprintf(out, "    addi %s, sp, 8\n", argv_r);
                }
            }
            if (ins->is_entry && g_uses_heap) {
                /* Seed __heap_ptr = &__heap once, right as execution
                   begins. */
                fprintf(out, "    la %s, %s\n", SCRATCH_RV, HEAP_SYM);
                fprintf(out, "    la %s, %s\n", SCRATCH_RV2, HEAP_PTR_SYM);
                fprintf(out, "    sd %s, 0(%s)\n", SCRATCH_RV, SCRATCH_RV2);
            }
            break;

        case OP_FRAME_OPEN: {
            /* Establishes this block's frame: reserve 16 bytes (the
               RISC-V calling convention requires sp to stay 16-byte
               aligned at all times, so a full aligned chunk is spent
               even though only 8 bytes are needed to save s0 -- same
               reasoning as AArch64's OP_FRAME_OPEN), save the caller's
               s0 into it, point s0 at the new frame base, then reserve
               frame_bytes more for this block's locals (if any). The
               save/establish step always runs, even for a block with
               zero locals of its own -- see the x86-64 OP_FRAME_OPEN
               case for why unconditional nesting support matters here. */
            fprintf(out, "    addi sp, sp, -16\n");
            fprintf(out, "    sd s0, 0(sp)\n");
            fprintf(out, "    mv s0, sp\n");
            int rounded = (ins->frame_bytes + 15) & ~15;
            if (rounded > 0) fprintf(out, "    addi sp, sp, -%d\n", rounded);
            break;
        }

        case OP_FRAME_CLOSE: {
            /* 'mv sp, s0' undoes the local reservation in one step
               (mirroring x86-64's 'mov rsp, rbp'), then the saved s0 is
               restored from the slot OP_FRAME_OPEN saved it in and the
               same 16-byte-aligned chunk reserved for it is popped. */
            fprintf(out, "    mv sp, s0\n");
            fprintf(out, "    ld s0, 0(sp)\n");
            fprintf(out, "    addi sp, sp, 16\n");
            break;
        }

        case OP_MOV:
            if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", reg__name(TARGET_RISCV, &ins->dst), ins->src.imm);
            } else if (is_mem_operand(ins->src.kind)) {
                /* mov SYM > rX; / mov local > rX; -- see the matching
                   x86-64 OP_MOV comment for the bug this fixes (a
                   memory-operand source used to fall through to the
                   plain-register branch below and read garbage out of
                   reg__name()). Same scratch-load pattern used
                   elsewhere on this backend for a memory source. */
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV);
                fprintf(out, "    mv %s, %s\n", reg__name(TARGET_RISCV, &ins->dst), SCRATCH_RV);
            } else {
                fprintf(out, "    mv %s, %s\n", reg__name(TARGET_RISCV, &ins->dst),
                        reg__name(TARGET_RISCV, &ins->src));
            }
            break;

        case OP_LOAD: {
            int sz = operand_mem_size(&ins->src);
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            /* RISC-V spells zero-extend and sign-extend as separate
               mnemonics with the 'u' suffix meaning unsigned/zero-
               extend (lbu/lhu/lwu) and its absence meaning sign-extend
               (lb/lh/lw) -- the opposite convention from x86-64/
               AArch64, where the *unsuffixed* mnemonic zero-extends.
               Default here is zero-extend (load_signed == 0), matching
               loadN's default on every other target; this used to
               default to sign-extend on RISC-V only, a real
               cross-target inconsistency this field now removes: the
               same Chard source now zero-extends a 'load8' identically
               on x86-64, AArch64, and RISC-V, rather than silently
               meaning something different depending which -target
               compiled it. 'ld' (64-bit) has no unsigned variant --
               RV64 loads the full 64-bit register either way, nothing
               to extend, same as x86-64/AArch64's 8-byte case. */
            const char *mn = ins->load_signed
                ? (sz == 1 ? "lb" : sz == 2 ? "lh" : sz == 4 ? "lw" : "ld")
                : (sz == 1 ? "lbu" : sz == 2 ? "lhu" : sz == 4 ? "lwu" : "ld");
            if (ins->src.kind == OPND_LOCAL) {
                /* dstreg doubles as chase scratch here: safe, since the
                   chase (if any) completes and is consumed by the final
                   load before dstreg's new value (the loaded local) is
                   written. Also safe to reuse as address-materialization
                   scratch for the same reason. */
                const char *base = riscv_local_base(out, &ins->src, dstreg);
                long safe_off;
                base = riscv_safe_offset(out, base, -(long)ins->src.local_offset, dstreg, &safe_off);
                fprintf(out, "    %s %s, %ld(%s)\n", mn, dstreg, safe_off, base);
                break;
            }
            if (ins->src.kind == OPND_ADDR) {
                /* Absolute address -- 'li' materializes the constant
                   directly into dstreg (safe to reuse as scratch here,
                   same reasoning as the OPND_LOCAL case above), then
                   load through it at offset 0. */
                fprintf(out, "    li %s, %ld\n", dstreg, ins->src.imm);
                fprintf(out, "    %s %s, 0(%s)\n", mn, dstreg, dstreg);
                break;
            }
            fprintf(out, "    la %s, %s\n", SCRATCH_RV, ins->src.sym);
            fprintf(out, "    %s %s, 0(%s)\n", mn, dstreg, SCRATCH_RV);
            break;
        }

        case OP_STORE: {
            int sz = operand_mem_size(&ins->dst);
            const char *srcreg = reg__name(TARGET_RISCV, &ins->src);
            const char *mn = sz == 1 ? "sb" : sz == 2 ? "sh" : sz == 4 ? "sw" : "sd";
            if (ins->dst.kind == OPND_LOCAL) {
                /* Unlike OP_LOAD, srcreg holds the value being stored,
                   so it can't double as chase/materialization scratch
                   here -- use SCRATCH_RV instead. */
                const char *base = riscv_local_base(out, &ins->dst, SCRATCH_RV);
                long safe_off;
                base = riscv_safe_offset(out, base, -(long)ins->dst.local_offset, SCRATCH_RV, &safe_off);
                fprintf(out, "    %s %s, %ld(%s)\n", mn, srcreg, safe_off, base);
                break;
            }
            if (ins->dst.kind == OPND_ADDR) {
                /* srcreg holds the value being stored, so (same
                   reasoning as the OPND_LOCAL branch above) it can't
                   double as address scratch -- use SCRATCH_RV. */
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, ins->dst.imm);
                fprintf(out, "    %s %s, 0(%s)\n", mn, srcreg, SCRATCH_RV);
                break;
            }
            fprintf(out, "    la %s, %s\n", SCRATCH_RV, ins->dst.sym);
            fprintf(out, "    %s %s, 0(%s)\n", mn, srcreg, SCRATCH_RV);
            break;
        }

        case OP_LEA: {
            /* Address of the symbol itself, computed directly into the
               destination register. A local's address is just base-off
               (a plain 'addi' with a negative immediate, or a short
               'addi' chain if it doesn't fit one), no 'la' pseudo-
               instruction needed since there's no linker symbol to
               resolve -- addressed relative to this block's own frame
               pointer (s0), established at OP_FRAME_OPEN, not sp
               directly, so nested blocks and any push/pop within a
               block that also has locals can't shift this address. A
               reference to a local declared in an enclosing block
               chases the saved-fp chain first via riscv_local_base,
               reusing the destination register as chase scratch --
               safe since the chase completes before the final 'addi'
               chain overwrites it with the actual address. */
            if (ins->src.kind == OPND_LOCAL) {
                const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
                const char *base = riscv_local_base(out, &ins->src, dstreg);
                riscv_emit_local_addr(out, dstreg, base, -(long)ins->src.local_offset);
                break;
            }
            fprintf(out, "    la %s, %s\n", reg__name(TARGET_RISCV, &ins->dst), ins->src.sym);
            break;
        }

        case OP_ADD: case OP_SUB: case OP_AND: case OP_OR: case OP_XOR:
        case OP_SHL: case OP_SHR: case OP_MUL: case OP_DIV: case OP_MOD: {
            /* OP_SHR is logical (zero-fill) shift-right, matching x86-64's
               'shr' and AArch64's 'lsr' -- 'srl' is RISC-V's logical
               shift, as opposed to 'sra' (arithmetic/sign-extending).
               Using 'sra' here would silently diverge from the other two
               backends: negative operands would sign-extend on RISC-V
               but zero-fill on x86/ARM for the exact same Chard source. */
            const char *mn = ins->op == OP_ADD ? "add" : ins->op == OP_SUB ? "sub" :
                              ins->op == OP_AND ? "and" : ins->op == OP_OR ? "or" :
                              ins->op == OP_XOR ? "xor" : ins->op == OP_SHL ? "sll" :
                              ins->op == OP_SHR ? "srl" : ins->op == OP_MUL ? "mul" :
                              ins->op == OP_DIV ? "div" : "rem";
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV);
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, SCRATCH_RV);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, ins->src.imm);
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, SCRATCH_RV);
            } else {
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, reg__name(TARGET_RISCV, &ins->src));
            }
            break;
        }

        case OP_NOT: case OP_NEG: {
            /* RISC-V provides both as standard two-register pseudo-ops
               (not rd, rs = xori rd, rs, -1; neg rd, rs = sub rd, x0,
               rs) -- no manual expansion needed, same as 'mv'/'li' for
               OP_MOV above. An immediate/memory src is materialized
               into scratch first, matching the OP_ADD/etc block. */
            const char *mn = ins->op == OP_NOT ? "not" : "neg";
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV);
                fprintf(out, "    %s %s, %s\n", mn, dstreg, SCRATCH_RV);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, ins->src.imm);
                fprintf(out, "    %s %s, %s\n", mn, dstreg, SCRATCH_RV);
            } else {
                fprintf(out, "    %s %s, %s\n", mn, dstreg, reg__name(TARGET_RISCV, &ins->src));
            }
            break;
        }

        case OP_ROTL: case OP_ROTR: {
            /* Zbb gives true rol/ror register-count instructions
               directly (no manual shift-pair synthesis needed, unlike
               targets without the extension) -- mirrors clz/ctz/cpop
               below, all four gated on the same Zbb baseline this
               project assumes for RISC-V bit-manipulation ops. */
            const char *mn = ins->op == OP_ROTL ? "rol" : "ror";
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV);
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, SCRATCH_RV);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, ins->src.imm);
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, SCRATCH_RV);
            } else {
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, reg__name(TARGET_RISCV, &ins->src));
            }
            break;
        }

        case OP_POPCOUNT: {
            /* Zbb cpop: direct reg,reg popcount, no synthesis needed
               (unlike AArch64's cnt+addv detour through a SIMD
               register). */
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV);
                fprintf(out, "    cpop %s, %s\n", dstreg, SCRATCH_RV);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, ins->src.imm);
                fprintf(out, "    cpop %s, %s\n", dstreg, SCRATCH_RV);
            } else {
                fprintf(out, "    cpop %s, %s\n", dstreg, reg__name(TARGET_RISCV, &ins->src));
            }
            break;
        }

        case OP_CLZ: case OP_CTZ: {
            /* Zbb clz/ctz: both native, no rbit-style synthesis needed
               (unlike AArch64's ctz, which has to detour through rbit+
               clz since AArch64 lacks a native ctz). */
            const char *mn = ins->op == OP_CLZ ? "clz" : "ctz";
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV);
                fprintf(out, "    %s %s, %s\n", mn, dstreg, SCRATCH_RV);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, ins->src.imm);
                fprintf(out, "    %s %s, %s\n", mn, dstreg, SCRATCH_RV);
            } else {
                fprintf(out, "    %s %s, %s\n", mn, dstreg, reg__name(TARGET_RISCV, &ins->src));
            }
            break;
        }

        case OP_SEXT: case OP_ZEXT: {
            /* sext.b/sext.h are Zbb instructions -- consistent with the
               Zbb baseline this backend already assumes for rotl/rotr/
               popcount/clz/ctz above, so no new ISA-extension
               assumption is introduced here. The 32-bit sign-extend
               case (sext32) doesn't need Zbb at all: 'addiw rd, rs, 0'
               is a base-ISA (RV64I) pseudo-instruction that adds zero
               in 32-bit mode, which sign-extends the 32-bit result into
               the full 64-bit register as an ordinary side effect of
               32-bit-mode arithmetic -- the same free-sign-extension
               property RV64I's other 'w'-suffixed ops (addw/subw/etc)
               already have. Zero-extension is deliberately NOT routed
               through Zbb's zext.b/zext.h (zext.h needs Zbb, and Zbb
               offers nothing named for the 32-bit case at all) -- a
               plain andi mask works unconditionally for 8/16-bit on
               base RV64I, and 32-bit zero-extension is a two-instruction
               shift-left-then-logical-shift-right-by-32 (srli, unlike
               sra/srai, always shifts in zero bits), so the whole
               opcode needs nothing beyond Zbb+RV64I either way. */
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            const char *srcreg;
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV);
                srcreg = SCRATCH_RV;
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, ins->src.imm);
                srcreg = SCRATCH_RV;
            } else {
                srcreg = reg__name(TARGET_RISCV, &ins->src);
            }
            if (ins->op == OP_SEXT) {
                if (ins->elem_size == 4) fprintf(out, "    addiw %s, %s, 0\n", dstreg, srcreg);
                else fprintf(out, "    sext.%s %s, %s\n", ins->elem_size == 1 ? "b" : "h", dstreg, srcreg);
            } else if (ins->elem_size == 1) {
                /* andi's immediate is a 12-bit signed field -- 0xff
                   (255) fits comfortably, so this is the one zext
                   width that doesn't need the shift-pair below. */
                fprintf(out, "    andi %s, %s, 0xff\n", dstreg, srcreg);
            } else {
                /* 16 and 32-bit masks (0xffff, 0xffffffff) don't fit
                   andi's 12-bit immediate, so both go through the same
                   shift-left-then-logical-shift-right-by-(64-N) idiom:
                   srli (unlike sra/srai) always shifts in zero bits
                   regardless of the operand's sign, so this reliably
                   clears everything above bit N-1 without needing a
                   dedicated mask constant at all. */
                int shift = 64 - (ins->elem_size * 8);
                fprintf(out, "    slli %s, %s, %d\n", dstreg, srcreg, shift);
                fprintf(out, "    srli %s, %s, %d\n", dstreg, dstreg, shift);
            }
            break;
        }

        case OP_SAT_ADD: case OP_SAT_SUB: {
            /* RISC-V has no flags register (same reason OP_CMP above
               stashes operands and lets jcc emission synthesize the
               comparison), so overflow has to be detected manually
               from operand/result signs rather than read off a V/OF
               bit like the other two backends. Signed-overflow rule:
               add overflows iff both operands share a sign AND the
               result's sign differs from theirs; sub overflows iff the
               operands have *different* signs AND the result's sign
               differs from the minuend's -- both captured in one
               branch-free XOR-of-signs formulation: overflow occurred
               iff (dst_orig ^ result) has its top bit set AND
               (dst_orig ^ src) reads the right way for the op (add:
               same-sign operands; sub: differing-sign operands, i.e.
               dst_orig ^ (~src) for add's test reused with src negated
               in place first). Implemented as: save dst_orig and src
               into scratch before the op, compute the raw result, XOR
               dst_orig with result and dst_orig with (src, negated for
               sub) into two more scratch values, AND their sign bits
               together via a shift-right-63 + and, then select the
               clamp with that 0/1 flag -- no branch, matching the
               other two backends' branch-free approach even without
               a flags register to key off of. */
            const char *mn = ins->op == OP_SAT_ADD ? "add" : "sub";
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            const char *srcreg;
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV);
                srcreg = SCRATCH_RV;
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, ins->src.imm);
                srcreg = SCRATCH_RV;
            } else {
                srcreg = reg__name(TARGET_RISCV, &ins->src);
            }
            /* t2 = dst_orig (saved before the op clobbers dstreg). */
            fprintf(out, "    mv t2, %s\n", dstreg);
            fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, srcreg);
            if (ins->op == OP_SAT_SUB) {
                /* sub overflow test wants (dst_orig ^ src), i.e. the
                   operands' signs already differing -- srcreg is used
                   directly, no negation needed (unlike the comment's
                   general framing above, sub's own operand-sign test
                   is already dst_orig XOR src as-is). Note srcreg may
                   itself be SCRATCH_RV (t0) if src was mem/imm -- read
                   here, before t0 is repurposed as the overflow flag
                   below, so there's no collision. */
                fprintf(out, "    xor t1, t2, %s\n", srcreg);
            } else {
                fprintf(out, "    xor t1, t2, %s\n", srcreg);
                fprintf(out, "    not t1, t1\n"); /* add wants operands SAME sign: invert the differ-test */
            }
            fprintf(out, "    xor t0, t2, %s\n", dstreg); /* dst_orig ^ result: did the sign flip? */
            fprintf(out, "    and t0, t0, t1\n"); /* both conditions true (top bit) => real overflow */
            fprintf(out, "    srli t0, t0, 63\n"); /* isolate the combined sign bit -> 0/1 overflow flag, kept in t0 for the rest of this block */
            /* Clamp value: MAX if the (now-computed) result looks
               negative (overflow wrapped a too-large positive sum past
               INT64_MAX into negative-looking bits, so the true value
               needs to clamp back up to MAX), MIN if it looks positive
               (the mirror case, wrapping a too-negative sum back up
               past zero). t1 is read for its sign bit and immediately
               branched on -- unlike an earlier version of this code,
               which computed the sign into t1 and then clobbered t1
               with a sentinel constant *before* the branch that reads
               it, silently corrupting the test (t1's sentinel value,
               0x8000..., is never zero, so 'beqz t1' would always take
               the same path regardless of the real sign). Branching
               immediately after the srli sidesteps the collision
               instead of finding a 4th scratch register (RISC-V's
               backend only reserves t0/t1/t2 as free-for-codegen). */
            fprintf(out, "    srli t1, %s, 63\n", dstreg);
            int satid = g_label_counter++;
            fprintf(out, "    bnez t1, .Lsat_neg%d\n", satid);
            /* result non-negative here -> clamp target is MIN */
            fprintf(out, "    li t1, 1\n");
            fprintf(out, "    slli t1, t1, 63\n"); /* t1 = 0x8000000000000000 (INT64_MIN) */
            fprintf(out, "    j .Lsat_select%d\n", satid);
            fprintf(out, ".Lsat_neg%d:\n", satid);
            /* result negative here -> clamp target is MAX */
            fprintf(out, "    li t1, -1\n");
            fprintf(out, "    srli t1, t1, 1\n"); /* t1 = 0x7fffffffffffffff (INT64_MAX) */
            fprintf(out, ".Lsat_select%d:\n", satid);
            fprintf(out, "    beqz t0, .Lsat_done%d\n", satid);
            fprintf(out, "    mv %s, t1\n", dstreg);
            fprintf(out, ".Lsat_done%d:\n", satid);
            break;
        }

        case OP_CMP:
            /* RISC-V has no flags register; stash operands in fixed
               scratch regs and let jcc emission (below) synthesize the
               branch directly from these two values. */
            render_simple_operand(TARGET_RISCV, &ins->dst, sb, sizeof(sb));
            fprintf(out, "    mv %s, %s\n", SCRATCH_RV, reg__name(TARGET_RISCV, &ins->dst));
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV2);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV2, ins->src.imm);
            } else {
                fprintf(out, "    mv %s, %s\n", SCRATCH_RV2, reg__name(TARGET_RISCV, &ins->src));
            }
            break;

        case OP_JMP: fprintf(out, ins->dst.kind == OPND_REG ? "    jr %s\n" : "    j %s\n", ins->dst.kind == OPND_REG ? reg__name(TARGET_RISCV, &ins->dst) : ins->dst.sym); break;
        case OP_JE:  fprintf(out, "    beq %s, %s, %s\n", SCRATCH_RV, SCRATCH_RV2, ins->dst.sym); break;
        case OP_JNE: fprintf(out, "    bne %s, %s, %s\n", SCRATCH_RV, SCRATCH_RV2, ins->dst.sym); break;
        case OP_JG:  fprintf(out, "    bgt %s, %s, %s\n", SCRATCH_RV, SCRATCH_RV2, ins->dst.sym); break;
        case OP_JL:  fprintf(out, "    blt %s, %s, %s\n", SCRATCH_RV, SCRATCH_RV2, ins->dst.sym); break;
        case OP_JGE: fprintf(out, "    bge %s, %s, %s\n", SCRATCH_RV, SCRATCH_RV2, ins->dst.sym); break;
        case OP_JLE: fprintf(out, "    ble %s, %s, %s\n", SCRATCH_RV, SCRATCH_RV2, ins->dst.sym); break;
        case OP_JA:  fprintf(out, "    bgtu %s, %s, %s\n", SCRATCH_RV, SCRATCH_RV2, ins->dst.sym); break;
        case OP_JB:  fprintf(out, "    bltu %s, %s, %s\n", SCRATCH_RV, SCRATCH_RV2, ins->dst.sym); break;
        case OP_JAE: fprintf(out, "    bgeu %s, %s, %s\n", SCRATCH_RV, SCRATCH_RV2, ins->dst.sym); break;
        case OP_JBE: fprintf(out, "    bleu %s, %s, %s\n", SCRATCH_RV, SCRATCH_RV2, ins->dst.sym); break;

        case OP_ASSERT: {
            /* Mirrors the x86-64/AArch64 OP_ASSERT cases: assert_jmp_op
               (the inverted condition from invert_cond_op) fires when
               the asserted condition is false, so branch on it straight
               to the trap label -- against the same SCRATCH_RV/
               SCRATCH_RV2 pair OP_CMP just populated (see the OP_CMP
               case above) -- otherwise jump past to ok_label. 'ebreak'
               is RISC-V's trap instruction. */
            int id = g_label_counter++;
            char trap_label[MAX_SYMLEN], ok_label[MAX_SYMLEN];
            snprintf(trap_label, sizeof(trap_label), ".Lassert%d_trap", id);
            snprintf(ok_label, sizeof(ok_label), ".Lassert%d_ok", id);
            static const char *jmp_mnemonic[] = {
                [OP_JE]="beq", [OP_JNE]="bne", [OP_JG]="bgt", [OP_JL]="blt",
                [OP_JGE]="bge", [OP_JLE]="ble", [OP_JA]="bgtu", [OP_JB]="bltu",
                [OP_JAE]="bgeu", [OP_JBE]="bleu"
            };
            fprintf(out, "    %s %s, %s, %s\n", jmp_mnemonic[ins->assert_jmp_op], SCRATCH_RV, SCRATCH_RV2, trap_label);
            fprintf(out, "    j %s\n", ok_label);
            fprintf(out, "%s:\n", trap_label);
            fprintf(out, "    ebreak\n");
            fprintf(out, "%s:\n", ok_label);
            break;
        }

        case OP_CALL: fprintf(out, ins->dst.kind == OPND_REG ? "    jalr %s\n" : "    call %s\n", ins->dst.kind == OPND_REG ? reg__name(TARGET_RISCV, &ins->dst) : ins->dst.sym); break;
        case OP_RET:  fprintf(out, "    ret\n"); break;

        case OP_EXIT:
            /* src is either OPND_IMM (exit(N)) or OPND_REG (exit(rN)) --
               see parse-time check above. Register case uses 'mv'
               (RISC-V's register-to-register move pseudo-op) instead
               of 'li' (load-immediate). */
            if (g_libc_linked) {
                /* See the x86-64 OP_EXIT comment: must go through
                   libc's exit() so stdio gets flushed, not the raw
                   exit syscall. */
                if (ins->src.kind == OPND_REG)
                    fprintf(out, "    mv a0, %s\n", reg__name(TARGET_RISCV, &ins->src));
                else
                    fprintf(out, "    li a0, %ld\n", ins->src.imm);
                fprintf(out, "    call exit\n");
            } else {
                if (ins->src.kind == OPND_REG)
                    fprintf(out, "    mv a0, %s\n", reg__name(TARGET_RISCV, &ins->src));
                else
                    fprintf(out, "    li a0, %ld\n", ins->src.imm);
                fprintf(out, "    li a7, 93\n");
                fprintf(out, "    ecall\n");
            }
            break;

        case OP_HALT:
            /* 'wfi' suspends the hart until an interrupt is pending;
               wrapped in its own label+jump loop for the same reason
               as the other two backends' halt loops above -- a
               spurious wake falls back into another 'wfi' instead of
               falling through into whatever comes next in memory. */
            {
                int id = g_label_counter++;
                fprintf(out, ".Lhalt%d:\n", id);
                fprintf(out, "    wfi\n");
                fprintf(out, "    j .Lhalt%d\n", id);
            }
            break;

        case OP_STDOUT:
            /* write(1, msg, msg_len). For a global ascii symbol, length
               comes from the compiler-generated <name>_len symbol,
               visible in this same file. For a 'local ascii' buffer
               (src.kind == OPND_LOCAL), msg_len is instead a second
               real local (dst, set up by out()'s parsing) -- its
               address is computed the same fp-relative way OP_LEA
               already computes any local's address. riscv_local_base
               takes its chase scratch as a parameter (unlike AArch64's
               fixed SCRATCH_ARM), so a1/a2 themselves double as that
               scratch: each chase completes and is consumed by
               riscv_emit_local_addr before the other register's chase
               begins, so there's no aliasing between the two. */
            fprintf(out, "    li a0, 1\n");
            if (ins->src.kind == OPND_LOCAL) {
                const char *base = riscv_local_base(out, &ins->src, "a1");
                riscv_emit_local_addr(out, "a1", base, -(long)ins->src.local_offset);
                const char *lenbase = riscv_local_base(out, &ins->dst, "a2");
                riscv_emit_local_addr(out, "a2", lenbase, -(long)ins->dst.local_offset);
                fprintf(out, "    ld a2, 0(a2)\n");
            } else {
                fprintf(out, "    la a1, %s\n", ins->src.sym);
                fprintf(out, "    la a2, %s_len\n", ins->src.sym);
                fprintf(out, "    ld a2, 0(a2)\n");
            }
            fprintf(out, "    li a7, 64\n"); /* Linux RISC-V write syscall number */
            fprintf(out, "    ecall\n");
            break;

        case OP_ALLOC: {
            /* Bump allocator: dst = __heap_ptr (the block base); then
               __heap_ptr += size. */
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            fprintf(out, "    la %s, %s\n", SCRATCH_RV2, HEAP_PTR_SYM);
            fprintf(out, "    ld %s, 0(%s)\n", dstreg, SCRATCH_RV2);
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV);
                fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV, dstreg, SCRATCH_RV);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, ins->src.imm);
                fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV, dstreg, SCRATCH_RV);
            } else {
                fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV, dstreg, reg__name(TARGET_RISCV, &ins->src));
            }
            fprintf(out, "    sd %s, 0(%s)\n", SCRATCH_RV, SCRATCH_RV2);
            break;
        }

        case OP_HEAP_RESET:
            /* Reclaim the whole arena: __heap_ptr = &__heap. SCRATCH_RV2
               holds __heap_ptr's own address (so it can be written to),
               SCRATCH_RV holds the value (&__heap) being stored -- same
               two-register split OP_ALLOC above uses. */
            fprintf(out, "    la %s, %s\n", SCRATCH_RV2, HEAP_PTR_SYM);
            fprintf(out, "    la %s, %s\n", SCRATCH_RV, HEAP_SYM);
            fprintf(out, "    sd %s, 0(%s)\n", SCRATCH_RV, SCRATCH_RV2);
            break;

        case OP_ILOAD: {
            /* RISC-V has no scaled-register addressing mode, unlike
               x86/AArch64: the index is shifted into SCRATCH_RV, added
               to the base into SCRATCH_RV2, then loaded from offset 0.
               Shift amounts double as log2(elem_size) since all four
               widths are powers of two. See the OP_LOAD case above for
               why the mnemonic picks the 'u' suffix on load_signed==0
               and drops it on load_signed==1 -- this iloadN family
               already defaulted to zero-extend (lbu/lhu/lwu) even
               before that field existed, unlike plain loadN, which
               didn't. */
            const char *basereg = reg__name(TARGET_RISCV, &ins->base_reg);
            const char *idxreg = reg__name(TARGET_RISCV, &ins->idx_reg);
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            int sz = ins->elem_size;
            int shift = sz == 1 ? 0 : sz == 2 ? 1 : sz == 4 ? 2 : 3;
            const char *mn = ins->load_signed
                ? (sz == 1 ? "lb" : sz == 2 ? "lh" : sz == 4 ? "lw" : "ld")
                : (sz == 1 ? "lbu" : sz == 2 ? "lhu" : sz == 4 ? "lwu" : "ld");
            if (shift > 0) fprintf(out, "    slli %s, %s, %d\n", SCRATCH_RV, idxreg, shift);
            else fprintf(out, "    mv %s, %s\n", SCRATCH_RV, idxreg);
            fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV2, basereg, SCRATCH_RV);
            fprintf(out, "    %s %s, 0(%s)\n", mn, dstreg, SCRATCH_RV2);
            break;
        }

        case OP_BCMP: {
            /* bcmpN rDST, rPTR1, rPTR2, LEN -- see the x86-64 OP_BCMP
               case for the overall two-loop (elem_size chunks, then a
               byte tail) strategy; adapted to RISC-V's own load
               mnemonics and lack of scaled addressing. t0/t1 are the
               byte offset and remaining-length countdown; t3/t4 hold
               each side's loaded value in turn -- all four are outside
               the a0-a7/s1-s4 range Chard registers map to (see
               target_defs[TARGET_RISCV]), so none can alias
               rPTR1/rPTR2/rDST regardless of which Chard registers the
               caller chose for them. */
            const char *p1reg = reg__name(TARGET_RISCV, &ins->src);
            const char *p2reg = reg__name(TARGET_RISCV, &ins->base_reg);
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            int sz = ins->elem_size;
            const char *mn = sz == 1 ? "lbu" : sz == 2 ? "lhu" : sz == 4 ? "lwu" : "ld";

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

            fprintf(out, "    li %s, 0\n", dstreg); /* assume equal */
            fprintf(out, "    li t0, 0\n"); /* byte offset, shared by both loops */
            if (ins->len_reg.kind == OPND_IMM) {
                fprintf(out, "    li t1, %ld\n", ins->len_reg.imm);
            } else {
                fprintf(out, "    mv t1, %s\n", reg__name(TARGET_RISCV, &ins->len_reg));
            }

            if (sz > 1) {
                fprintf(out, "    j %s\n", chunkcheck_lbl);
                fprintf(out, "%s:\n", chunk_lbl);
                fprintf(out, "    add t3, %s, t0\n", p1reg);
                fprintf(out, "    %s t3, 0(t3)\n", mn);
                fprintf(out, "    add t4, %s, t0\n", p2reg);
                fprintf(out, "    %s t4, 0(t4)\n", mn);
                fprintf(out, "    beq t3, t4, %s\n", chunkskip_lbl);
                fprintf(out, "    li %s, 1\n", dstreg);
                fprintf(out, "%s:\n", chunkskip_lbl);
                fprintf(out, "    addi t0, t0, %d\n", sz);
                fprintf(out, "    addi t1, t1, -%d\n", sz);
                fprintf(out, "%s:\n", chunkcheck_lbl);
                fprintf(out, "    li t3, %d\n", sz);
                fprintf(out, "    bge t1, t3, %s\n", chunk_lbl);
            }

            fprintf(out, "    j %s\n", tailcheck_lbl);
            fprintf(out, "%s:\n", tail_lbl);
            fprintf(out, "    add t3, %s, t0\n", p1reg);
            fprintf(out, "    lbu t3, 0(t3)\n");
            fprintf(out, "    add t4, %s, t0\n", p2reg);
            fprintf(out, "    lbu t4, 0(t4)\n");
            fprintf(out, "    beq t3, t4, %s\n", tailskip_lbl);
            fprintf(out, "    li %s, 1\n", dstreg);
            fprintf(out, "%s:\n", tailskip_lbl);
            fprintf(out, "    addi t0, t0, 1\n");
            fprintf(out, "    addi t1, t1, -1\n");
            fprintf(out, "%s:\n", tailcheck_lbl);
            fprintf(out, "    bgtz t1, %s\n", tail_lbl);
            fprintf(out, "%s:\n", done_lbl);
            break;
        }

        case OP_BCOPY: {
            /* bcopyN rDST, rSRC, LEN -- RISC-V counterpart to the
               x86-64/AArch64 OP_BCOPY cases (see the x86-64 one for
               the overall strategy relative to bcmpN); adapted from
               this target's own OP_BCMP immediately above the same
               way. t0/t1 are the shared byte offset and spilled
               countdown, t3 stages each chunk's value between the
               load from rSRC and the store to rDST -- same
               store-width mnemonics (sb/sh/sw/sd) OP_ISTORE below
               already uses for each elem_size. */
            const char *srcreg = reg__name(TARGET_RISCV, &ins->src);
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            int sz = ins->elem_size;
            const char *loadmn = sz == 1 ? "lbu" : sz == 2 ? "lhu" : sz == 4 ? "lwu" : "ld";
            const char *storemn = sz == 1 ? "sb" : sz == 2 ? "sh" : sz == 4 ? "sw" : "sd";

            char id[16];
            snprintf(id, sizeof(id), "%d", g_label_counter++);
            char chunk_lbl[40], chunkcheck_lbl[40];
            char tail_lbl[40], tailcheck_lbl[40], done_lbl[40];
            snprintf(chunk_lbl, sizeof(chunk_lbl), ".Lbcopy_chunk%s", id);
            snprintf(chunkcheck_lbl, sizeof(chunkcheck_lbl), ".Lbcopy_chunkcheck%s", id);
            snprintf(tail_lbl, sizeof(tail_lbl), ".Lbcopy_tail%s", id);
            snprintf(tailcheck_lbl, sizeof(tailcheck_lbl), ".Lbcopy_tailcheck%s", id);
            snprintf(done_lbl, sizeof(done_lbl), ".Lbcopy_done%s", id);

            fprintf(out, "    li t0, 0\n"); /* byte offset, shared by both loops */
            if (ins->len_reg.kind == OPND_IMM) {
                fprintf(out, "    li t1, %ld\n", ins->len_reg.imm);
            } else {
                fprintf(out, "    mv t1, %s\n", reg__name(TARGET_RISCV, &ins->len_reg));
            }

            if (sz > 1) {
                fprintf(out, "    j %s\n", chunkcheck_lbl);
                fprintf(out, "%s:\n", chunk_lbl);
                fprintf(out, "    add t3, %s, t0\n", srcreg);
                fprintf(out, "    %s t3, 0(t3)\n", loadmn);
                fprintf(out, "    add t4, %s, t0\n", dstreg);
                fprintf(out, "    %s t3, 0(t4)\n", storemn);
                fprintf(out, "    addi t0, t0, %d\n", sz);
                fprintf(out, "    addi t1, t1, -%d\n", sz);
                fprintf(out, "%s:\n", chunkcheck_lbl);
                fprintf(out, "    li t3, %d\n", sz);
                fprintf(out, "    bge t1, t3, %s\n", chunk_lbl);
            }

            fprintf(out, "    j %s\n", tailcheck_lbl);
            fprintf(out, "%s:\n", tail_lbl);
            fprintf(out, "    add t3, %s, t0\n", srcreg);
            fprintf(out, "    lbu t3, 0(t3)\n");
            fprintf(out, "    add t4, %s, t0\n", dstreg);
            fprintf(out, "    sb t3, 0(t4)\n");
            fprintf(out, "    addi t0, t0, 1\n");
            fprintf(out, "    addi t1, t1, -1\n");
            fprintf(out, "%s:\n", tailcheck_lbl);
            fprintf(out, "    bgtz t1, %s\n", tail_lbl);
            fprintf(out, "%s:\n", done_lbl);
            break;
        }


        case OP_ISTORE: {
            const char *basereg = reg__name(TARGET_RISCV, &ins->base_reg);
            const char *idxreg = reg__name(TARGET_RISCV, &ins->idx_reg);
            int sz = ins->elem_size;
            int shift = sz == 1 ? 0 : sz == 2 ? 1 : sz == 4 ? 2 : 3;
            const char *mn = sz == 1 ? "sb" : sz == 2 ? "sh" : sz == 4 ? "sw" : "sd";
            const char *srcreg;
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV3);
                srcreg = SCRATCH_RV3;
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV3, ins->src.imm);
                srcreg = SCRATCH_RV3;
            } else {
                srcreg = reg__name(TARGET_RISCV, &ins->src);
            }
            if (shift > 0) fprintf(out, "    slli %s, %s, %d\n", SCRATCH_RV, idxreg, shift);
            else fprintf(out, "    mv %s, %s\n", SCRATCH_RV, idxreg);
            fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV2, basereg, SCRATCH_RV);
            fprintf(out, "    %s %s, 0(%s)\n", mn, srcreg, SCRATCH_RV2);
            break;
        }

        case OP_HFIELD_LOAD: {
            /* rDST = *(rBASE + const_offset). RISC-V's load instructions
               already take an immediate offset directly (ld rd,
               imm(rs1)) when it fits 12 bits; otherwise the address is
               materialized into dstreg first (safe to clobber -- it's
               about to receive the loaded value anyway). */
            const char *basereg = reg__name(TARGET_RISCV, &ins->base_reg);
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            int sz = ins->elem_size;
            const char *mn = sz == 1 ? "lbu" : sz == 2 ? "lhu" : sz == 4 ? "lwu" : "ld";
            long safe_off;
            const char *addrbase = riscv_safe_offset(out, basereg, (long)ins->const_offset, dstreg, &safe_off);
            fprintf(out, "    %s %s, %ld(%s)\n", mn, dstreg, safe_off, addrbase);
            break;
        }

        case OP_HFIELD_STORE: {
            const char *basereg = reg__name(TARGET_RISCV, &ins->base_reg);
            int sz = ins->elem_size;
            const char *mn = sz == 1 ? "sb" : sz == 2 ? "sh" : sz == 4 ? "sw" : "sd";
            const char *srcreg;
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV3);
                srcreg = SCRATCH_RV3;
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV3, ins->src.imm);
                srcreg = SCRATCH_RV3;
            } else {
                srcreg = reg__name(TARGET_RISCV, &ins->src);
            }
            long safe_off;
            /* srcreg may already be SCRATCH_RV3 (the value to store),
               so address materialization must not clobber it -- use
               SCRATCH_RV instead, which is otherwise free here. */
            const char *addrbase = riscv_safe_offset(out, basereg, (long)ins->const_offset, SCRATCH_RV, &safe_off);
            fprintf(out, "    %s %s, %ld(%s)\n", mn, srcreg, safe_off, addrbase);
            break;
        }

        case OP_XLOAD: {
            /* rDST = *(base + idx*scale + disp). RISC-V has neither
               scaled-register addressing (see OP_ILOAD) nor a combined
               scale+displacement form, so this composes the same two
               tricks each already uses on their own: shift+add builds
               base+idx*scale into SCRATCH_RV2 (t1) exactly like
               OP_ILOAD's own base+idx*elem_size, then
               riscv_safe_offset applies disp on top of that -- the
               same helper OP_HFIELD_LOAD uses for its displacement-
               only case, materializing into SCRATCH_RV (t0) if disp
               doesn't fit a 12-bit immediate. t0 is free again by
               this point (its only use was as dstreg for the shift
               step's *source*, and slli/mv below write into t1
               instead), so it's safe to hand riscv_safe_offset here. */
            const char *basereg = reg__name(TARGET_RISCV, &ins->base_reg);
            const char *idxreg = reg__name(TARGET_RISCV, &ins->idx_reg);
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            int sz = ins->elem_size;
            int scale = ins->xaddr_scale;
            int shift = scale == 1 ? 0 : scale == 2 ? 1 : scale == 4 ? 2 : 3;
            const char *mn = sz == 1 ? "lbu" : sz == 2 ? "lhu" : sz == 4 ? "lwu" : "ld";
            if (shift > 0) fprintf(out, "    slli %s, %s, %d\n", SCRATCH_RV2, idxreg, shift);
            else fprintf(out, "    mv %s, %s\n", SCRATCH_RV2, idxreg);
            fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV2, basereg, SCRATCH_RV2);
            long safe_off;
            const char *addrbase = riscv_safe_offset(out, SCRATCH_RV2, (long)ins->const_offset, SCRATCH_RV, &safe_off);
            fprintf(out, "    %s %s, %ld(%s)\n", mn, dstreg, safe_off, addrbase);
            break;
        }

        case OP_XSTORE: {
            const char *basereg = reg__name(TARGET_RISCV, &ins->base_reg);
            const char *idxreg = reg__name(TARGET_RISCV, &ins->idx_reg);
            int sz = ins->elem_size;
            int scale = ins->xaddr_scale;
            int shift = scale == 1 ? 0 : scale == 2 ? 1 : scale == 4 ? 2 : 3;
            const char *mn = sz == 1 ? "sb" : sz == 2 ? "sh" : sz == 4 ? "sw" : "sd";
            const char *srcreg;
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV3);
                srcreg = SCRATCH_RV3;
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV3, ins->src.imm);
                srcreg = SCRATCH_RV3;
            } else {
                srcreg = reg__name(TARGET_RISCV, &ins->src);
            }
            if (shift > 0) fprintf(out, "    slli %s, %s, %d\n", SCRATCH_RV2, idxreg, shift);
            else fprintf(out, "    mv %s, %s\n", SCRATCH_RV2, idxreg);
            fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV2, basereg, SCRATCH_RV2);
            long safe_off;
            /* srcreg may be SCRATCH_RV3 (the value to store), distinct
               from both SCRATCH_RV/SCRATCH_RV2 used for the address --
               same non-collision reasoning as OP_HFIELD_STORE. */
            const char *addrbase = riscv_safe_offset(out, SCRATCH_RV2, (long)ins->const_offset, SCRATCH_RV, &safe_off);
            fprintf(out, "    %s %s, %ld(%s)\n", mn, srcreg, safe_off, addrbase);
            break;
        }

        case OP_PTRADD: case OP_PTRSUB: {
            /* rDST = base +/- (idx*scale + disp), or rDST = base +/-
               disp when idx_reg is absent (the bracket-less form --
               see its parse-time comment). Builds the address into
               dstreg directly (never dereferenced): the idx*scale term
               reuses OP_XLOAD's own shift+add composition (RISC-V has
               no scaled-register addressing mode at all, load or
               otherwise -- see OP_ILOAD), then disp is added via 'li'
               into SCRATCH_RV followed by 'add'/'sub' -- simpler than
               riscv_safe_offset's 12-bit-immediate-or-materialize
               logic since 'li' already handles any 64-bit constant
               directly and there's no load/store immediate-encoding
               limit to work around here, only a plain register
               destination. */
            const char *basereg = reg__name(TARGET_RISCV, &ins->base_reg);
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            int is_sub = (ins->op == OP_PTRSUB);
            long off = ins->const_offset;

            const char *addr_src = basereg; /* what disp still needs to be added to */
            if (ins->idx_reg.kind == OPND_REG) {
                const char *idxreg = reg__name(TARGET_RISCV, &ins->idx_reg);
                int scale = ins->xaddr_scale;
                int shift = scale == 1 ? 0 : scale == 2 ? 1 : scale == 4 ? 2 : 3;
                if (shift > 0) fprintf(out, "    slli %s, %s, %d\n", dstreg, idxreg, shift);
                else fprintf(out, "    mv %s, %s\n", dstreg, idxreg);
                if (!is_sub) fprintf(out, "    add %s, %s, %s\n", dstreg, basereg, dstreg);
                else fprintf(out, "    sub %s, %s, %s\n", dstreg, basereg, dstreg);
                addr_src = dstreg; /* disp now applies on top of dstreg, not basereg */
            }

            if (off != 0) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, off);
                if (!is_sub) fprintf(out, "    add %s, %s, %s\n", dstreg, addr_src, SCRATCH_RV);
                else fprintf(out, "    sub %s, %s, %s\n", dstreg, addr_src, SCRATCH_RV);
            } else if (addr_src != dstreg) {
                /* No index term and no displacement: dst is just a
                   plain copy of base. */
                fprintf(out, "    mv %s, %s\n", dstreg, basereg);
            }
            break;
        }

        case OP_LALOAD: {
            /* rDST = local array element at idx. RISC-V has no scaled-
               register addressing (same limitation OP_ILOAD already
               works around above): the array's fp-relative base is
               computed into SCRATCH_RV2 first (chasing the saved-fp
               chain first if frames_up > 0), then either a shifted
               index is added on top of it (register index) or a
               literal index folds directly into the load's own
               offset(reg) form, skipping the shift/add entirely since
               there's nothing to scale at runtime in that case. */
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            int sz = ins->elem_size;
            int shift = sz == 1 ? 0 : sz == 2 ? 1 : sz == 4 ? 2 : 3;
            const char *mn = sz == 1 ? "lbu" : sz == 2 ? "lhu" : sz == 4 ? "lwu" : "ld";

            const char *fpreg = "s0";
            if (ins->src.frames_up > 0) {
                fprintf(out, "    ld %s, 0(s0)\n", SCRATCH_RV2);
                for (int k = 1; k < ins->src.frames_up; k++)
                    fprintf(out, "    ld %s, 0(%s)\n", SCRATCH_RV2, SCRATCH_RV2);
                fpreg = SCRATCH_RV2;
            }

            if (ins->idx_reg.kind == OPND_IMM) {
                long total_off = ins->src.local_offset - ins->idx_reg.imm * sz;
                long safe_off;
                /* dstreg is free to use as materialization scratch --
                   it's about to receive the loaded value. */
                const char *addrbase = riscv_safe_offset(out, fpreg, -total_off, dstreg, &safe_off);
                fprintf(out, "    %s %s, %ld(%s)\n", mn, dstreg, safe_off, addrbase);
            } else {
                const char *idxreg = reg__name(TARGET_RISCV, &ins->idx_reg);
                if (shift > 0) fprintf(out, "    slli %s, %s, %d\n", SCRATCH_RV, idxreg, shift);
                else fprintf(out, "    mv %s, %s\n", SCRATCH_RV, idxreg);
                /* base - local_offset + (idx<<shift): subtract the
                   constant offset from fpreg first, then add the
                   scaled index -- SCRATCH_RV2 is reused as the running
                   address accumulator since fpreg may already alias it
                   (frames_up > 0 case) and this sequencing still
                   produces the right final value either way. The
                   subtraction itself may need more than one addi if
                   local_offset doesn't fit a single 12-bit immediate. */
                riscv_emit_local_addr(out, SCRATCH_RV2, fpreg, -(long)ins->src.local_offset);
                fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV2, SCRATCH_RV2, SCRATCH_RV);
                fprintf(out, "    %s %s, 0(%s)\n", mn, dstreg, SCRATCH_RV2);
            }
            break;
        }

        case OP_LASTORE: {
            int sz = ins->elem_size;
            int shift = sz == 1 ? 0 : sz == 2 ? 1 : sz == 4 ? 2 : 3;
            const char *mn = sz == 1 ? "sb" : sz == 2 ? "sh" : sz == 4 ? "sw" : "sd";
            const char *srcreg;
            if (is_mem_operand(ins->src.kind)) {
                emit_riscv_load_scratch(out, &ins->src, SCRATCH_RV3);
                srcreg = SCRATCH_RV3;
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV3, ins->src.imm);
                srcreg = SCRATCH_RV3;
            } else {
                srcreg = reg__name(TARGET_RISCV, &ins->src);
            }

            const char *fpreg = "s0";
            if (ins->dst.frames_up > 0) {
                fprintf(out, "    ld %s, 0(s0)\n", SCRATCH_RV2);
                for (int k = 1; k < ins->dst.frames_up; k++)
                    fprintf(out, "    ld %s, 0(%s)\n", SCRATCH_RV2, SCRATCH_RV2);
                fpreg = SCRATCH_RV2;
            }

            if (ins->idx_reg.kind == OPND_IMM) {
                long total_off = ins->dst.local_offset - ins->idx_reg.imm * sz;
                long safe_off;
                /* srcreg may be SCRATCH_RV3 (the value to store), so
                   materialize into SCRATCH_RV instead, which is free
                   here (the idx_reg==OPND_IMM path never touches it). */
                const char *addrbase = riscv_safe_offset(out, fpreg, -total_off, SCRATCH_RV, &safe_off);
                fprintf(out, "    %s %s, %ld(%s)\n", mn, srcreg, safe_off, addrbase);
            } else {
                const char *idxreg = reg__name(TARGET_RISCV, &ins->idx_reg);
                if (shift > 0) fprintf(out, "    slli %s, %s, %d\n", SCRATCH_RV, idxreg, shift);
                else fprintf(out, "    mv %s, %s\n", SCRATCH_RV, idxreg);
                riscv_emit_local_addr(out, SCRATCH_RV2, fpreg, -(long)ins->dst.local_offset);
                fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV2, SCRATCH_RV2, SCRATCH_RV);
                fprintf(out, "    %s %s, 0(%s)\n", mn, srcreg, SCRATCH_RV2);
            }
            break;
        }

        case OP_PUSH:
            /* RISC-V has no push instruction: decrement sp then store.
               The offset comes from stack_slot_size() (see the table
               up top) rather than being repeated here as a literal.
               A float source (f1-f7/s1-s7) needs 'fsd' (float store
               doubleword), not 'sd' -- RISC-V's F/D extensions use a
               completely separate register file (f0-f31, ABI-named
               fa0.../fs0.../ft0...) from the integer file, and 'sd'
               only knows how to store an integer register; attempting
               'sd fa0, 0(sp)' is simply not valid RISC-V assembly.
               'fsd' always stores the full 8 bytes regardless of
               whether the value is a true f64 or a narrower f32 held
               in the low 32 bits (see the x86-64 OP_PUSH comment for
               why: keeps every spilled slot the same size so the pop
               side never has to know which file it's restoring). */
            if (ins->src.kind == OPND_REG && ins->src.is_float) {
                const char *freg = reg__name(TARGET_RISCV, &ins->src);
                fprintf(out, "    addi sp, sp, -%d\n", stack_slot_size(TARGET_RISCV));
                fprintf(out, "    fsd %s, 0(sp)\n", freg);
            } else if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, ins->src.imm);
                fprintf(out, "    addi sp, sp, -%d\n", stack_slot_size(TARGET_RISCV));
                fprintf(out, "    sd %s, 0(sp)\n", SCRATCH_RV);
            } else {
                fprintf(out, "    addi sp, sp, -%d\n", stack_slot_size(TARGET_RISCV));
                fprintf(out, "    sd %s, 0(sp)\n", reg__name(TARGET_RISCV, &ins->src));
            }
            break;

        case OP_POP:
            /* Mirrors OP_PUSH: load then increment sp by the same
               stack_slot_size() amount the push side reserved. A float
               destination uses 'fld' (float load doubleword), the
               'fsd' counterpart, for the same reason described above. */
            if (ins->dst.kind == OPND_REG && ins->dst.is_float) {
                const char *freg = reg__name(TARGET_RISCV, &ins->dst);
                fprintf(out, "    fld %s, 0(sp)\n", freg);
                fprintf(out, "    addi sp, sp, %d\n", stack_slot_size(TARGET_RISCV));
            } else {
                fprintf(out, "    ld %s, 0(sp)\n", ins->dst.kind == OPND_REG ?
                        reg__name(TARGET_RISCV, &ins->dst) : SCRATCH_RV);
                fprintf(out, "    addi sp, sp, %d\n", stack_slot_size(TARGET_RISCV));
            }
            break;

        /* Computes the full effective address of a memory operand
           (symbol or local) into SCRATCH_RV, mirroring
           emit_aarch64_addr_into_scratch -- needed here for the same
           reason: RISC-V's amo*.d/amoswap.d instructions (like
           AArch64's ldxr/stxr) take a bare '(reg)' with no immediate
           offset, unlike ordinary ld/sd, which fold a local's offset
           directly into the instruction (see emit_riscv_load_scratch). */
        case OP_ATOMIC_ADD: case OP_ATOMIC_SUB: case OP_ATOMIC_AND:
        case OP_ATOMIC_OR: case OP_ATOMIC_XOR: case OP_ATOMIC_SWAP: {
            int sz = operand_mem_size(&ins->dst);
            const char *width_suffix = sz == 4 ? ".w" : ".d"; /* RV64A has no byte/half amo* forms */
            if (sz == 1 || sz == 2) fail("atomic ops on RISC-V require a 4- or 8-byte location (RV64A has no byte/halfword atomics)");
            /* Ordering (ins->mem_order -- see mem_order_t): RISC-V's
               amo* instructions carry their ordering as two independent
               trailing bits on the mnemonic itself -- .aq (acquire) and
               .rl (release), combined as .aqrl -- rather than as
               separate load-/store-side instruction choices the way
               AArch64's ldxr/ldaxr + stxr/stlxr split works. So this is
               a single suffix string, not the use_acquire/use_release
               instruction-selection AArch64's codegen above does:
                 SEQ_CST/ACQ_REL -> ".aqrl" (both bits -- pre-existing,
                                     always-used suffix from before the
                                     ordering parameter existed, so
                                     unannotated atomics are unaffected)
                 ACQUIRE         -> ".aq"
                 RELEASE         -> ".rl"
                 RELAXED         -> ""     (no ordering bits at all) */
            const char *order_suffix = ins->mem_order == MEM_ORDER_SEQ_CST || ins->mem_order == MEM_ORDER_ACQ_REL ? ".aqrl"
                                      : ins->mem_order == MEM_ORDER_ACQUIRE ? ".aq"
                                      : ins->mem_order == MEM_ORDER_RELEASE ? ".rl"
                                      : "";
            char suffix[8];
            snprintf(suffix, sizeof(suffix), "%s%s", width_suffix, order_suffix);
            if (ins->dst.kind == OPND_LOCAL) {
                const char *base = riscv_local_base(out, &ins->dst, SCRATCH_RV);
                riscv_emit_local_addr(out, SCRATCH_RV, base, -(long)ins->dst.local_offset);
            } else {
                fprintf(out, "    la %s, %s\n", SCRATCH_RV, ins->dst.sym);
            }
            const char *valreg = reg__name(TARGET_RISCV, &ins->result_reg);
            if (ins->op == OP_ATOMIC_SWAP) {
                if (ins->src.kind == OPND_IMM) {
                    fprintf(out, "    li %s, %ld\n", SCRATCH_RV2, ins->src.imm);
                    fprintf(out, "    amoswap%s %s, %s, (%s)\n", suffix, valreg, SCRATCH_RV2, SCRATCH_RV);
                } else {
                    fprintf(out, "    amoswap%s %s, %s, (%s)\n", suffix, valreg, reg__name(TARGET_RISCV, &ins->src), SCRATCH_RV);
                }
            } else {
                const char *mn = ins->op == OP_ATOMIC_ADD ? "amoadd" : ins->op == OP_ATOMIC_SUB ? "amoadd" :
                                  ins->op == OP_ATOMIC_AND ? "amoand" : ins->op == OP_ATOMIC_OR ? "amoor" : "amoxor";
                if (ins->src.kind == OPND_IMM) {
                    long v = ins->op == OP_ATOMIC_SUB ? -ins->src.imm : ins->src.imm;
                    fprintf(out, "    li %s, %ld\n", SCRATCH_RV2, v);
                } else if (ins->op == OP_ATOMIC_SUB) {
                    fprintf(out, "    neg %s, %s\n", SCRATCH_RV2, reg__name(TARGET_RISCV, &ins->src));
                } else {
                    fprintf(out, "    mv %s, %s\n", SCRATCH_RV2, reg__name(TARGET_RISCV, &ins->src));
                }
                fprintf(out, "    %s%s %s, %s, (%s)\n", mn, suffix, valreg, SCRATCH_RV2, SCRATCH_RV);
            }
            break;
        }

        case OP_ATOMIC_MAX: case OP_ATOMIC_MIN: {
            /* RV64A has a native signed fetch-and-max/min instruction
               (amomax.d/amomin.d) -- unlike AND/OR/XOR/SWAP above,
               which is why this is its own case rather than folding
               into the mn-selection ladder there: amomax/amomin are
               real opcodes with the exact same '(addr), reg, reg' shape
               as amoadd/etc, so the same address-computation and
               ordering-suffix logic applies unchanged, just with a
               different mnemonic and no separate negate/no-op branch
               needed (SRC is used as-is either way). Signed only
               (amomax/amomin, not the amomaxu/amominu unsigned forms),
               matching the opcode_t comment on OP_ATOMIC_MAX/
               OP_ATOMIC_MIN. */
            int sz = operand_mem_size(&ins->dst);
            const char *width_suffix = sz == 4 ? ".w" : ".d";
            if (sz == 1 || sz == 2) fail("atomic ops on RISC-V require a 4- or 8-byte location (RV64A has no byte/halfword atomics)");
            const char *order_suffix = ins->mem_order == MEM_ORDER_SEQ_CST || ins->mem_order == MEM_ORDER_ACQ_REL ? ".aqrl"
                                      : ins->mem_order == MEM_ORDER_ACQUIRE ? ".aq"
                                      : ins->mem_order == MEM_ORDER_RELEASE ? ".rl"
                                      : "";
            char suffix[8];
            snprintf(suffix, sizeof(suffix), "%s%s", width_suffix, order_suffix);
            if (ins->dst.kind == OPND_LOCAL) {
                const char *base = riscv_local_base(out, &ins->dst, SCRATCH_RV);
                riscv_emit_local_addr(out, SCRATCH_RV, base, -(long)ins->dst.local_offset);
            } else {
                fprintf(out, "    la %s, %s\n", SCRATCH_RV, ins->dst.sym);
            }
            const char *valreg = reg__name(TARGET_RISCV, &ins->result_reg);
            const char *mn = ins->op == OP_ATOMIC_MAX ? "amomax" : "amomin";
            if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV2, ins->src.imm);
            } else {
                fprintf(out, "    mv %s, %s\n", SCRATCH_RV2, reg__name(TARGET_RISCV, &ins->src));
            }
            fprintf(out, "    %s%s %s, %s, (%s)\n", mn, suffix, valreg, SCRATCH_RV2, SCRATCH_RV);
            break;
        }

        case OP_ATOMIC_CAS: {
            /* No single-instruction CAS on RV64A: synthesized from the
               lr.d/sc.d reservation pair, same load-linked/store-
               conditional retry loop AArch64's ldxr/stxr uses below
               (RISC-V's own recommended CAS idiom). Ordering: lr/sc
               (like every amo* instruction) carry .aq/.rl as mnemonic
               suffix bits -- lr gets .aq when the requested ordering
               needs an acquire barrier on the read, sc gets .rl when it
               needs a release barrier on the write, mirroring the same
               acquire-on-load/release-on-store split the RMW ops above
               and AArch64's ldaxr/stlxr both use, just expressed as a
               suffix instead of a different mnemonic. SEQ_CST/ACQ_REL
               set both bits on both instructions (matching the RMW
               ops' pre-existing, always-used behavior, so unannotated
               'atom=' is unaffected); RELAXED sets neither. */
            int sz = operand_mem_size(&ins->dst);
            int use_acquire = ins->mem_order == MEM_ORDER_SEQ_CST || ins->mem_order == MEM_ORDER_ACQ_REL || ins->mem_order == MEM_ORDER_ACQUIRE;
            int use_release = ins->mem_order == MEM_ORDER_SEQ_CST || ins->mem_order == MEM_ORDER_ACQ_REL || ins->mem_order == MEM_ORDER_RELEASE;
            char lr[16], sc[16];
            snprintf(lr, sizeof(lr), "%s%s", sz == 4 ? "lr.w" : "lr.d", use_acquire ? ".aq" : "");
            snprintf(sc, sizeof(sc), "%s%s", sz == 4 ? "sc.w" : "sc.d", use_release ? ".rl" : "");
            if (sz == 1 || sz == 2) fail("atom= on RISC-V requires a 4- or 8-byte location (RV64A has no byte/halfword atomics)");
            if (ins->dst.kind == OPND_LOCAL) {
                const char *base = riscv_local_base(out, &ins->dst, SCRATCH_RV);
                riscv_emit_local_addr(out, SCRATCH_RV, base, -(long)ins->dst.local_offset);
            } else {
                fprintf(out, "    la %s, %s\n", SCRATCH_RV, ins->dst.sym);
            }
            const char *expreg = reg__name(TARGET_RISCV, &ins->cas_expected);
            const char *desreg = reg__name(TARGET_RISCV, &ins->cas_desired);
            const char *resultreg = reg__name(TARGET_RISCV, &ins->result_reg);
            char loop_lbl[32], fail_lbl[32], done_lbl[32];
            snprintf(loop_lbl, sizeof(loop_lbl), ".Lcas_loop%d", g_label_counter);
            snprintf(fail_lbl, sizeof(fail_lbl), ".Lcas_fail%d", g_label_counter);
            snprintf(done_lbl, sizeof(done_lbl), ".Lcas_done%d", g_label_counter);
            g_label_counter++;

            fprintf(out, "%s:\n", loop_lbl);
            fprintf(out, "    %s %s, (%s)\n", lr, SCRATCH_RV2, SCRATCH_RV);
            fprintf(out, "    bne %s, %s, %s\n", SCRATCH_RV2, expreg, fail_lbl);
            fprintf(out, "    %s %s, %s, (%s)\n", sc, SCRATCH_RV3, desreg, SCRATCH_RV);
            fprintf(out, "    bnez %s, %s\n", SCRATCH_RV3, loop_lbl); /* sc.d failed (lost reservation) -- retry */
            fprintf(out, "    li %s, 1\n", resultreg);
            fprintf(out, "    j %s\n", done_lbl);
            fprintf(out, "%s:\n", fail_lbl);
            fprintf(out, "    li %s, 0\n", resultreg);
            fprintf(out, "%s:\n", done_lbl);
            break;
        }

        case OP_I2S: {
            /* i2s rSRC > rBUF, rLEN. Same digit algorithm as the other
               two targets (see x86-64's comment for the overall
               shape); RV64I has neither a combined div+mod
               instruction (div and rem are separate ops, each
               dividing the same operands again) nor a reg+reg
               addressing mode (every load/store needs its effective
               address computed into a register first), so this ends
               up needing more address-scratch traffic than the other
               two backends for the same work.

               Register roles:
                 t0 (SCRATCH_RV)  - base pointer, copied from rBUF up
                              front, same reasoning as the other
                              targets' base-pointer copy
                 t1 (SCRATCH_RV2) - magnitude/quotient/remainder work
                              during the digit loop; an address-
                              computation scratch and the reversal's
                              first byte temp afterward
                 t2 (SCRATCH_RV3) - the running write index during the
                              digit loop, ending it holding the total
                              byte count; becomes the reversal's 'hi'
                              index afterward
                 rBUF's own physical register - divisor-10 scratch
                              during the digit loop, then the
                              reversal's 'lo' index (safe to reuse,
                              rBUF is a documented write target here)
                 rLEN's own physical register - address-computation /
                              quotient scratch during the digit loop,
                              then the reversal's second byte temp

               Between explicit addressing and no divmod fusion, this
               target has the least register slack of the three, so
               (as with AArch64, for the same underlying reason) the
               total digit count and the reversal's first byte temp
               are each given a small self-contained stack slot for
               the reversal's duration rather than trying to force
               everything through registers alone. */
            const char *bufreg = reg__name(TARGET_RISCV, &ins->dst);
            const char *lenreg = reg__name(TARGET_RISCV, &ins->len_reg);
            const char *srcreg = reg__name(TARGET_RISCV, &ins->src);

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

            fprintf(out, "    mv %s, %s\n", SCRATCH_RV, bufreg);
            fprintf(out, "    mv %s, %s\n", SCRATCH_RV2, srcreg);
            fprintf(out, "    bge %s, zero, %s\n", SCRATCH_RV2, pos_lbl);
            fprintf(out, "    li %s, 45\n", bufreg); /* '-' */
            fprintf(out, "    sb %s, 0(%s)\n", bufreg, SCRATCH_RV);
            fprintf(out, "    neg %s, %s\n", SCRATCH_RV2, SCRATCH_RV2);
            fprintf(out, "    li %s, 1\n", SCRATCH_RV3);
            fprintf(out, "    j %s\n", magready_lbl);
            fprintf(out, "%s:\n", pos_lbl);
            fprintf(out, "    li %s, 0\n", SCRATCH_RV3);
            fprintf(out, "%s:\n", magready_lbl);
            fprintf(out, "    bnez %s, %s\n", SCRATCH_RV2, loop_lbl2);
            fprintf(out, "    li %s, 48\n", bufreg); /* '0' */
            fprintf(out, "    add %s, %s, %s\n", lenreg, SCRATCH_RV, SCRATCH_RV3);
            fprintf(out, "    sb %s, 0(%s)\n", bufreg, lenreg);
            fprintf(out, "    addi %s, %s, 1\n", SCRATCH_RV3, SCRATCH_RV3);
            fprintf(out, "    j %s\n", digitsdone_lbl);
            fprintf(out, "%s:\n", loop_lbl2);
            fprintf(out, "    li %s, 10\n", bufreg);
            fprintf(out, "    div %s, %s, %s\n", lenreg, SCRATCH_RV2, bufreg);
            fprintf(out, "    rem %s, %s, %s\n", SCRATCH_RV2, SCRATCH_RV2, bufreg);
            fprintf(out, "    addi %s, %s, 48\n", SCRATCH_RV2, SCRATCH_RV2);
            fprintf(out, "    add %s, %s, %s\n", bufreg, SCRATCH_RV, SCRATCH_RV3);
            fprintf(out, "    sb %s, 0(%s)\n", SCRATCH_RV2, bufreg);
            fprintf(out, "    addi %s, %s, 1\n", SCRATCH_RV3, SCRATCH_RV3);
            fprintf(out, "    mv %s, %s\n", SCRATCH_RV2, lenreg);
            fprintf(out, "    bnez %s, %s\n", SCRATCH_RV2, loop_lbl2);
            fprintf(out, "%s:\n", digitsdone_lbl);
            /* Spill the total count (and later the first swap byte)
               so t1/t2 are free to become 'lo peek'/'hi' scratch and
               the swap's two live byte temps during the reversal. */
            fprintf(out, "    addi sp, sp, -16\n");
            fprintf(out, "    sd %s, 0(sp)\n", SCRATCH_RV3);
            /* Peek-based lo: buf[0] is '-' iff the number was negative. */
            fprintf(out, "    li %s, 0\n", bufreg);
            fprintf(out, "    lb %s, 0(%s)\n", SCRATCH_RV2, SCRATCH_RV);
            fprintf(out, "    li %s, 45\n", SCRATCH_RV3);
            fprintf(out, "    bne %s, %s, %s\n", SCRATCH_RV2, SCRATCH_RV3, revbounds_lbl);
            fprintf(out, "    li %s, 1\n", bufreg);
            fprintf(out, "%s:\n", revbounds_lbl);
            fprintf(out, "    ld %s, 0(sp)\n", SCRATCH_RV3);
            fprintf(out, "    addi %s, %s, -1\n", SCRATCH_RV3, SCRATCH_RV3);
            fprintf(out, "    j %s\n", revcheck_lbl);
            fprintf(out, "%s:\n", revswap_lbl);
            fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV2, SCRATCH_RV, bufreg);
            fprintf(out, "    lb %s, 0(%s)\n", SCRATCH_RV2, SCRATCH_RV2);
            fprintf(out, "    sd %s, 8(sp)\n", SCRATCH_RV2);
            fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV2, SCRATCH_RV, SCRATCH_RV3);
            fprintf(out, "    lb %s, 0(%s)\n", lenreg, SCRATCH_RV2);
            fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV2, SCRATCH_RV, bufreg);
            fprintf(out, "    sb %s, 0(%s)\n", lenreg, SCRATCH_RV2);
            fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV2, SCRATCH_RV, SCRATCH_RV3);
            fprintf(out, "    ld %s, 8(sp)\n", lenreg);
            fprintf(out, "    sb %s, 0(%s)\n", lenreg, SCRATCH_RV2);
            fprintf(out, "    addi %s, %s, 1\n", bufreg, bufreg);
            fprintf(out, "    addi %s, %s, -1\n", SCRATCH_RV3, SCRATCH_RV3);
            fprintf(out, "%s:\n", revcheck_lbl);
            fprintf(out, "    blt %s, %s, %s\n", bufreg, SCRATCH_RV3, revswap_lbl);
            fprintf(out, "    ld %s, 0(sp)\n", lenreg);
            fprintf(out, "    addi sp, sp, 16\n");
            break;
        }

        case OP_S2I: {
            /* s2i rBUF, rLEN > rDST. rBUF/rLEN are read-only inputs
               (see the pin-check comment on OP_S2I), so unlike i2s
               their physical registers stay untouched throughout --
               everything works out of t0/t1/t2, which is enough since
               there's no reversal to fund here, only a single forward
               accumulate pass.

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
            const char *bufreg = reg__name(TARGET_RISCV, &ins->dst);
            const char *lenreg = reg__name(TARGET_RISCV, &ins->len_reg);
            const char *dstreg = reg__name(TARGET_RISCV, &ins->result_reg);

            char id[16];
            snprintf(id, sizeof(id), "%d", g_label_counter++);
            char loop_lbl2[40], negcheck_lbl[40], done_lbl2[40];
            snprintf(loop_lbl2, sizeof(loop_lbl2), ".Ls2i_loop%s", id);
            snprintf(negcheck_lbl, sizeof(negcheck_lbl), ".Ls2i_negcheck%s", id);
            snprintf(done_lbl2, sizeof(done_lbl2), ".Ls2i_done%s", id);

            fprintf(out, "    li %s, 0\n", dstreg); /* accumulator must start at 0 -- a pre-existing gap independent of the INT64_MIN fix above, found while verifying it: dstreg otherwise carries in whatever it last held */
            fprintf(out, "    li %s, 0\n", SCRATCH_RV);
            fprintf(out, "    beqz %s, %s\n", lenreg, done_lbl2);
            fprintf(out, "    lb %s, 0(%s)\n", SCRATCH_RV2, bufreg);
            fprintf(out, "    li %s, 45\n", SCRATCH_RV3);
            fprintf(out, "    bne %s, %s, %s\n", SCRATCH_RV2, SCRATCH_RV3, loop_lbl2);
            fprintf(out, "    li %s, 1\n", SCRATCH_RV);
            fprintf(out, "%s:\n", loop_lbl2);
            fprintf(out, "    bge %s, %s, %s\n", SCRATCH_RV, lenreg, negcheck_lbl);
            fprintf(out, "    add %s, %s, %s\n", SCRATCH_RV3, bufreg, SCRATCH_RV);
            fprintf(out, "    lb %s, 0(%s)\n", SCRATCH_RV2, SCRATCH_RV3);
            fprintf(out, "    addi %s, %s, -48\n", SCRATCH_RV2, SCRATCH_RV2);
            fprintf(out, "    li %s, 10\n", SCRATCH_RV3);
            fprintf(out, "    mul %s, %s, %s\n", dstreg, dstreg, SCRATCH_RV3);
            fprintf(out, "    sub %s, %s, %s\n", dstreg, dstreg, SCRATCH_RV2); /* accumulate negatively -- see comment above */
            fprintf(out, "    addi %s, %s, 1\n", SCRATCH_RV, SCRATCH_RV);
            fprintf(out, "    j %s\n", loop_lbl2);
            fprintf(out, "%s:\n", negcheck_lbl);
            fprintf(out, "    lb %s, 0(%s)\n", SCRATCH_RV2, bufreg);
            fprintf(out, "    li %s, 45\n", SCRATCH_RV3);
            fprintf(out, "    beq %s, %s, %s\n", SCRATCH_RV2, SCRATCH_RV3, done_lbl2); /* already negative, and correctly so */
            fprintf(out, "    neg %s, %s\n", dstreg, dstreg); /* positive path: flip back */
            fprintf(out, "%s:\n", done_lbl2);
            break;
        }

        case OP_RAW:
            fprintf(out, "    %s\n", ins->raw_text);
            break;

        case OP_RAWDATA: {
            /* Same .byte/.half/.word/.dword choice the SEC_DATA
               'is_data_array' loop above uses for 'data', dropped inline
               into .text instead. GAS is happy to mix data directives
               into .text (same reasoning as the x86-64 backend).
               For raw_data_is_float, dsize is only ever 4 or 8, so
               .word/.dword still cover it -- values go through
               float__bits/double__bits as hex, same as the SEC_DATA
               float-array loop above. */
            const char *dsz = ins->raw_data_size == 1 ? ".byte" :
                               ins->raw_data_size == 2 ? ".half" :
                               ins->raw_data_size == 4 ? ".word" : ".dword";
            fprintf(out, "    %s ", dsz);
            for (int v = 0; v < ins->raw_data_nvals; v++) {
                if (ins->raw_data_is_float) {
                    if (ins->raw_data_size == 4)
                        fprintf(out, "%s0x%08x", v == 0 ? "" : ", ", float__bits(ins->raw_data_fvals[v]));
                    else
                        fprintf(out, "%s0x%016llx", v == 0 ? "" : ", ", (unsigned long long)double__bits(ins->raw_data_fvals[v]));
                } else {
                    /* '&label' entries -- see riscv_emit_int_val's comment
                       and the 'bytes' parsing branch's own comment. */
                    riscv_emit_int_val(out, ins->raw_data_vals[v], ins->raw_data_val_is_label, ins->raw_data_val_labels, v, v == 0);
                }
            }
            fprintf(out, "\n");
            break;
        }

        case OP_FENCE:
            /* fence PRED,SUCC: RISC-V's memory barrier takes an explicit
               predecessor/successor set (each r/w) rather than a named
               strength -- fence rw,rw (both sets full) is the standard
               "everything before is visible before everything after, to
               everyone" barrier, matching mfence's role on x86-64 and
               dmb ish's on AArch64. Weaker orderings narrow which sides
               of the barrier matter:
                 SEQ_CST/ACQ_REL -> fence rw,rw (both directions --
                                     pre-existing, always-used form from
                                     before the ordering parameter
                                     existed, so unannotated 'fence' is
                                     unaffected)
                 ACQUIRE         -> fence r,rw (only 'nothing after may
                                     move before a preceding read'
                                     matters)
                 RELEASE         -> fence rw,w (only 'nothing before may
                                     move after a following write'
                                     matters)
                 RELAXED         -> no instruction (a relaxed fence
                                     establishes no ordering by
                                     definition -- emitted as a comment
                                     rather than silently nothing, same
                                     reasoning as AArch64's relaxed-fence
                                     case above) */
            if (ins->mem_order == MEM_ORDER_RELAXED)
                fprintf(out, "    # fence %%relaxed -- no ordering requested, no instruction needed\n");
            else if (ins->mem_order == MEM_ORDER_ACQUIRE)
                fprintf(out, "    fence r,rw\n");
            else if (ins->mem_order == MEM_ORDER_RELEASE)
                fprintf(out, "    fence rw,w\n");
            else
                fprintf(out, "    fence rw,rw\n");
            break;

        /* Floats (RV64D). f1-f7 map to fa0-fa6 (the D-extension
           calling-convention argument registers, reused here purely as
           a convenient distinct register file -- Chard doesn't itself
           follow the RISC-V calling convention for user code), always
           holding a double -- same "always compute at f64" model as
           the other two targets (see the OP_FADD family comment in the
           opcode_t enum). ft0 is fscratch. RISC-V has no float-
           immediate-load instruction either: a literal's bit pattern
           is built in the integer scratch register (li handles an
           arbitrary 64-bit immediate directly, unlike AArch64's
           16-bit-at-a-time movz/movk), then moved across register
           files with fmv.d.x. */
        case OP_FMOV:
            /* f32 dst: fmv.w.x (32-bit int-to-float bit move) fed by a
               32-bit bit pattern, and fmv.s for reg-to-reg -- distinct
               mnemonics from the f64 forms, unlike FADD/etc below where
               only the .s/.d suffix changes. */
            if (ins->src.kind == OPND_IMM) {
                if (ins->dst.is_f32) {
                    fprintf(out, "    li %s, 0x%08x\n", SCRATCH_RV, float__bits(ins->src.fimm));
                    fprintf(out, "    fmv.w.x %s, %s\n", reg__name(TARGET_RISCV, &ins->dst), SCRATCH_RV);
                } else {
                    fprintf(out, "    li %s, 0x%016llx\n", SCRATCH_RV, (unsigned long long)double__bits(ins->src.fimm));
                    fprintf(out, "    fmv.d.x %s, %s\n", reg__name(TARGET_RISCV, &ins->dst), SCRATCH_RV);
                }
            } else {
                fprintf(out, "    %s %s, %s\n", ins->dst.is_f32 ? "fmv.s" : "fmv.d",
                        reg__name(TARGET_RISCV, &ins->dst), reg__name(TARGET_RISCV, &ins->src));
            }
            break;

        case OP_FLOAD: {
            /* dst.is_f32 decides the load width directly -- flw for
               s-registers, fld for f-registers, straight into dstreg
               with no fcvt widen step (sregs==fregs on RISC-V, so the
               register name itself doesn't change, only the
               instruction's width). */
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            const char *mn = ins->dst.is_f32 ? "flw" : "fld";
            if (ins->src.kind == OPND_LOCAL) {
                const char *base = riscv_local_base(out, &ins->src, SCRATCH_RV);
                long safe_off;
                base = riscv_safe_offset(out, base, -(long)ins->src.local_offset, SCRATCH_RV, &safe_off);
                fprintf(out, "    %s %s, %ld(%s)\n", mn, dstreg, safe_off, base);
            } else {
                fprintf(out, "    la %s, %s\n", SCRATCH_RV, ins->src.sym);
                fprintf(out, "    %s %s, 0(%s)\n", mn, dstreg, SCRATCH_RV);
            }
            break;
        }

        case OP_FSTORE: {
            /* Mirrors OP_FLOAD: src.is_f32 picks fsw vs fsd directly,
               no fcvt narrow step. */
            const char *srcreg = reg__name(TARGET_RISCV, &ins->src);
            const char *mn = ins->src.is_f32 ? "fsw" : "fsd";
            if (ins->dst.kind == OPND_LOCAL) {
                const char *base = riscv_local_base(out, &ins->dst, SCRATCH_RV);
                long safe_off;
                base = riscv_safe_offset(out, base, -(long)ins->dst.local_offset, SCRATCH_RV, &safe_off);
                fprintf(out, "    %s %s, %ld(%s)\n", mn, srcreg, safe_off, base);
            } else {
                fprintf(out, "    la %s, %s\n", SCRATCH_RV, ins->dst.sym);
                fprintf(out, "    %s %s, 0(%s)\n", mn, srcreg, SCRATCH_RV);
            }
            break;
        }

        case OP_VLOAD: {
            /* No packed 128-bit load on RISC-V (same "no fixed-width
               packed register" gap as every other vN op -- see
               OP_VADD), so this unrolls into two ordinary scalar
               fld.d's: lane 0 at the array's base address (element 0,
               same address OP_FLOAD would use for a plain symbol),
               lane 1 at base+8 (element 1 -- declare_local_array lays
               out 'element i is at (base_offset - i*size_bytes)' in
               its own local_offset numbering, which is the same thing
               as '+8 bytes higher in memory' once translated through
               the '[fp - local_offset]' real-address convention every
               other local access already uses -- see that function's
               comment for the derivation). dst (an f-register, one
               scalar slot on this target) receives lane 0's real
               value; lane 1 is loaded into fscratch and then
               discarded, mirroring the arithmetic vN family's own
               "lane 1 is a real instruction, not silently dropped,
               its result just has nowhere Chard-visible to go on this
               target" pattern (see OP_VADD's RISC-V case). Shares the
               same one-time stderr note the other vN ops already
               print. */
            if (!g_riscv_vadd_warned) {
                g_riscv_vadd_warned = 1;
                fprintf(stderr,
                    "note: 'vadd'/'vsub'/'vmul'/'vdiv'/'vmin'/'vmax'/'vsqrt'/'vabs'/'vneg'/"
                    "'vload'/'vstore' on --target=riscv are NOT real SIMD -- RISC-V has no "
                    "fixed-width packed-double register (unlike x86-64 SSE2/AArch64 NEON), only "
                    "the Vector extension (RVV), which needs runtime vsetvli setup this compiler "
                    "does not emit. Arithmetic/unary ops fall back to two sequential scalar "
                    "instructions per op; vload/vstore fall back to two sequential scalar "
                    "fld.d/fsd.d's at the array's two 8-byte-apart element addresses -- correct "
                    "result either way, but no parallelism and no speed benefit over the plain "
                    "scalar f-ops on this target.\n");
            }
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            const char *fscratch = target_defs[TARGET_RISCV].fscratch;
            if (ins->src.kind == OPND_LOCAL) {
                const char *base = riscv_local_base(out, &ins->src, SCRATCH_RV);
                long safe_off0, safe_off1;
                const char *base0 = riscv_safe_offset(out, base, -(long)ins->src.local_offset, SCRATCH_RV, &safe_off0);
                fprintf(out, "    fld %s, %ld(%s)\n", dstreg, safe_off0, base0);
                /* Re-chase for lane 1: riscv_safe_offset may have
                   consumed SCRATCH_RV as its materialized base above,
                   so the offset for element 1 is recomputed fresh
                   against the *original* base (re-derived via
                   riscv_local_base again -- cheap, a single 's0' or a
                   short fp chase, not worth caching across the two
                   lanes). */
                base = riscv_local_base(out, &ins->src, SCRATCH_RV);
                const char *base1 = riscv_safe_offset(out, base, -(long)ins->src.local_offset + 8, SCRATCH_RV, &safe_off1);
                fprintf(out, "    fld %s, %ld(%s)\n", fscratch, safe_off1, base1);
            } else {
                fprintf(out, "    la %s, %s\n", SCRATCH_RV, ins->src.sym);
                fprintf(out, "    fld %s, 0(%s)\n", dstreg, SCRATCH_RV);
                fprintf(out, "    fld %s, 8(%s)\n", fscratch, SCRATCH_RV);
            }
            break;
        }

        case OP_VSTORE: {
            /* Mirrors OP_VLOAD: two scalar fsd.d's instead of fld.d's,
               same address derivation and shared warning. src (one
               real f-register on this target) supplies lane 0's
               value; lane 1 has no second source value to write at
               all -- unlike OP_VLOAD's discarded-but-real second
               load, there is no second live f-register here for a
               "lane 1" to come from, so only element 0 of the target
               array is written. This is a genuine, unavoidable
               information loss on this target: a real vN register on
               x86-64/AArch64 carries two lanes, but RISC-V has never
               had anywhere to put a second one (see OP_VADD's opcode_t
               comment), so 'vstore' here can only ever persist the one
               lane this target actually has -- element 1 of the
               destination array is left however it already was
               (uninitialized garbage for a fresh local, unless the
               program itself has already written it). Noted in the
               shared stderr warning above as part of the general
               "not real SIMD" caveat rather than a second, more alarming
               note, since a portable Chard program that only ever
               populates a vN register via vload (not by hand-writing
               two separate f-registers, which the vN family has no
               instruction for anyway) round-trips correctly: vload's
               own lane 1 was equally discarded going in, so there was
               never a second real value to lose in the first place on
               this target. */
            const char *srcreg = reg__name(TARGET_RISCV, &ins->src);
            if (ins->dst.kind == OPND_LOCAL) {
                const char *base = riscv_local_base(out, &ins->dst, SCRATCH_RV);
                long safe_off;
                base = riscv_safe_offset(out, base, -(long)ins->dst.local_offset, SCRATCH_RV, &safe_off);
                fprintf(out, "    fsd %s, %ld(%s)\n", srcreg, safe_off, base);
            } else {
                fprintf(out, "    la %s, %s\n", SCRATCH_RV, ins->dst.sym);
                fprintf(out, "    fsd %s, 0(%s)\n", srcreg, SCRATCH_RV);
            }
            break;
        }

        case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
            /* .d -> .s suffix swap for f32 (mirrors OP_VADD's own
               comment about this being mnemonic-only on RISC-V);
               dstreg is unchanged either way since sregs==fregs here.
               Immediate materialization uses fmv.w.x + a 32-bit bit
               pattern for f32 (same as FMOV's narrowed path), fmv.d.x
               + the full 64-bit pattern for f64. */
            int f32 = ins->dst.is_f32;
            const char *mn = ins->op == OP_FADD ? (f32 ? "fadd.s" : "fadd.d") :
                              ins->op == OP_FSUB ? (f32 ? "fsub.s" : "fsub.d") :
                              ins->op == OP_FMUL ? (f32 ? "fmul.s" : "fmul.d") : (f32 ? "fdiv.s" : "fdiv.d");
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            const char *fscratch = target_defs[TARGET_RISCV].fscratch;
            if (ins->src.kind == OPND_IMM) {
                if (f32) {
                    fprintf(out, "    li %s, 0x%08x\n", SCRATCH_RV, float__bits(ins->src.fimm));
                    fprintf(out, "    fmv.w.x %s, %s\n", fscratch, SCRATCH_RV);
                } else {
                    fprintf(out, "    li %s, 0x%016llx\n", SCRATCH_RV, (unsigned long long)double__bits(ins->src.fimm));
                    fprintf(out, "    fmv.d.x %s, %s\n", fscratch, SCRATCH_RV);
                }
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, fscratch);
            } else {
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, reg__name(TARGET_RISCV, &ins->src));
            }
            break;
        }

        case OP_VADD: case OP_VSUB: case OP_VMUL: case OP_VDIV:
        case OP_VMIN: case OP_VMAX: {
            /* RISC-V's f0-f31 hold one f64 each -- no packed-double
               register the way x86 xmm/AArch64 d-v aliasing gives
               OP_FADD's f1-f8 a second lane for OP_VADD/etc. Real SIMD
               here is the Vector extension (RVV): a separate v0-v31
               file needing runtime vsetvli, not a mnemonic swap.

               Fallback: unroll vadd/vsub/vmul/vdiv/vmin/vmax into two
               scalar f*.d instructions. Lane 0 is the real value; lane
               1 is a real second instruction (not dropped), computed
               as <identity> op <identity> and discarded. Identity is
               0.0 for add/sub/mul/min/max (every f-register write in
               this compiler zero-extends, so the real targets' unused
               upper lane is also 0.0). vdiv is the exception: 0.0/0.0
               is NaN under IEEE-754, matching what real SSE2 divpd
               also produces in that situation -- not a divergence.

               vsqrt/vabs/vneg (unary, below) share this gap and
               warning but need no identity-op-identity dance, just the
               unary op on a synthesized 0.0 in lane 1.

               Not real SIMD -- two sequential scalar ops, no
               parallelism -- but keeps vN mnemonics portable and
               correct everywhere at the cost of the perf win on this
               target. Warned once per compile (shared flag across all
               nine ops), not fail()'d: numeric result is correct even
               though instruction selection isn't what these mnemonics
               promise. */
            if (!g_riscv_vadd_warned) {
                g_riscv_vadd_warned = 1;
                fprintf(stderr,
                    "note: 'vadd'/'vsub'/'vmul'/'vdiv'/'vmin'/'vmax'/'vsqrt'/'vabs'/'vneg' on "
                    "--target=riscv are NOT real SIMD -- RISC-V has no fixed-width packed-double "
                    "register (unlike x86-64 SSE2/AArch64 NEON), only the Vector extension "
                    "(RVV), which needs runtime vsetvli setup this compiler does not emit. "
                    "Falling back to two sequential scalar instructions per op (lane 0 = your "
                    "values, lane 1 = the identity op'd with itself for the two-operand forms, "
                    "or applied to a synthesized 0.0 for the unary forms -- matching what an "
                    "unused upper lane always holds on the other two targets: 0.0 for "
                    "add/sub/mul/min/max/sqrt/abs, NaN for div, -0.0 for neg) -- correct result, "
                    "but no parallelism and no speed benefit over the plain scalar f-ops on this "
                    "target.\n");
            }
            {
                const char *mn = ins->op == OP_VADD ? "fadd.d" : ins->op == OP_VSUB ? "fsub.d" :
                                  ins->op == OP_VMUL ? "fmul.d" : ins->op == OP_VDIV ? "fdiv.d" :
                                  ins->op == OP_VMIN ? "fmin.d" : "fmax.d";
                const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
                const char *srcreg = reg__name(TARGET_RISCV, &ins->src);
                const char *fscratch = target_defs[TARGET_RISCV].fscratch;
                /* Lane 0: the real op, same as plain fadd.d/fsub.d/etc. */
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, srcreg);
                /* Lane 1: identity op identity (0.0+0.0 / 0.0-0.0 /
                   0.0*0.0 / 0.0/0.0), emitted explicitly rather than
                   optimized away so the two-lane unroll stays visible
                   in the output. */
                fprintf(out, "    fcvt.d.w %s, zero\n", fscratch);
                fprintf(out, "    %s %s, %s, %s\n", mn, fscratch, fscratch, fscratch);
            }
            break;
        }

        case OP_VSQRT: case OP_VABS: case OP_VNEG: {
            /* Same "no fixed-width packed register on RISC-V" gap as
               OP_VADD/etc just above -- see that case's comment for
               the full rationale and the shared one-time warning
               (triggered from either case block, whichever fires
               first). Unary, though, so the fallback is one register
               simpler: lane 0 is the real fsqrt.d/fabs.d/fneg.d, lane
               1 is the same unary op applied to a synthesized 0.0
               (sqrt(0.0) = 0.0, |0.0| = 0.0, -0.0 for fneg -- see the
               OP_VSQRT/OP_VABS/OP_VNEG opcode_t comments for why each
               of those matches what x86-64/AArch64 leave in their own
               discarded upper lane), rather than combining two live
               operands the way the destructive vN family's fallback
               does. */
            if (!g_riscv_vadd_warned) {
                g_riscv_vadd_warned = 1;
                fprintf(stderr,
                    "note: 'vadd'/'vsub'/'vmul'/'vdiv'/'vmin'/'vmax'/'vsqrt'/'vabs'/'vneg' on "
                    "--target=riscv are NOT real SIMD -- RISC-V has no fixed-width packed-double "
                    "register (unlike x86-64 SSE2/AArch64 NEON), only the Vector extension "
                    "(RVV), which needs runtime vsetvli setup this compiler does not emit. "
                    "Falling back to two sequential scalar instructions per op (lane 0 = your "
                    "values, lane 1 = the identity op'd with itself for the two-operand forms, "
                    "or applied to a synthesized 0.0 for the unary forms -- matching what an "
                    "unused upper lane always holds on the other two targets: 0.0 for "
                    "add/sub/mul/min/max/sqrt/abs, NaN for div, -0.0 for neg) -- correct result, "
                    "but no parallelism and no speed benefit over the plain scalar f-ops on this "
                    "target.\n");
            }
            {
                const char *mn = ins->op == OP_VSQRT ? "fsqrt.d" : ins->op == OP_VABS ? "fabs.d" : "fneg.d";
                const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
                const char *srcreg = reg__name(TARGET_RISCV, &ins->src);
                const char *fscratch = target_defs[TARGET_RISCV].fscratch;
                /* Lane 0: the real op, same as plain fsqrt.d/fabs.d/fneg.d. */
                fprintf(out, "    %s %s, %s\n", mn, dstreg, srcreg);
                /* Lane 1: the unary op applied to a synthesized 0.0,
                   emitted explicitly rather than optimized away so the
                   two-lane unroll stays visible in the output -- same
                   pattern as the destructive vN family's identity-op-
                   identity lane 1 just above, minus the second operand. */
                fprintf(out, "    fcvt.d.w %s, zero\n", fscratch);
                fprintf(out, "    %s %s, %s\n", mn, fscratch, fscratch);
            }
            break;
        }

        case OP_VDUP: {
            /* Same "no fixed-width packed register on RISC-V" gap as
               OP_VADD/OP_VSQRT/etc above -- see those cases for the
               full rationale and the shared one-time warning. Simplest
               fallback in the whole vN family: no per-lane computation
               at all (not even an identity op), just the same value
               written twice -- lane 0 is dstreg = srcreg (a plain
               register-to-register double copy, fmv.d), lane 1 is
               the discarded scratch set to that same value, emitted
               explicitly rather than skipped so the two-instruction
               shape stays visible/consistent with the rest of the vN
               family's fallback, even though (unlike vsqrt/vadd/etc)
               there's no actual second lane being computed here for a
               later op to read -- vdup's "lane 1" on this target is
               purely cosmetic bookkeeping, not a real packed write. */
            if (!g_riscv_vadd_warned) {
                g_riscv_vadd_warned = 1;
                fprintf(stderr,
                    "note: 'vadd'/'vsub'/'vmul'/'vdiv'/'vmin'/'vmax'/'vsqrt'/'vabs'/'vneg'/'vdup'/"
                    "'vfma' on --target=riscv are NOT real SIMD -- RISC-V has no fixed-width "
                    "packed-double register (unlike x86-64 SSE2/AArch64 NEON), only the Vector "
                    "extension (RVV), which needs runtime vsetvli setup this compiler does not "
                    "emit. Falling back to two sequential scalar instructions per op (lane 0 = "
                    "your values, lane 1 = the identity op'd with itself for the two-operand "
                    "forms, applied to a synthesized 0.0 for the unary forms, the same broadcast "
                    "value again for vdup, or 0*0+0 for vfma -- matching what an unused upper "
                    "lane always holds on the other two targets: 0.0 for "
                    "add/sub/mul/min/max/sqrt/abs/fma, NaN for div, -0.0 for neg, the broadcast "
                    "value itself for dup) -- correct result, but no parallelism and no speed "
                    "benefit over the plain scalar f-ops on this target.\n");
            }
            {
                const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
                const char *srcreg = reg__name(TARGET_RISCV, &ins->src);
                const char *fscratch = target_defs[TARGET_RISCV].fscratch;
                /* Lane 0: the real broadcast source, copied into dst. */
                fprintf(out, "    fmv.d %s, %s\n", dstreg, srcreg);
                /* Lane 1: the same value again, into the (discarded)
                   scratch register -- see the case comment above for
                   why this is cosmetic rather than a real second lane
                   write on this target. */
                fprintf(out, "    fmv.d %s, %s\n", fscratch, srcreg);
            }
            break;
        }

        case OP_VFMA: {
            /* Same "no fixed-width packed register on RISC-V" gap as
               OP_VADD/OP_VDUP/etc above -- see those cases for the full
               rationale and the shared one-time warning. Lane 0 is a
               direct fmadd.d dst, fA, fB, fC -- RISC-V's fused
               multiply-add already takes three distinct source
               registers (same shape as OP_FMA's own RISC-V case), so
               unlike x86-64/AArch64's OP_VFMA cases there's no
               destructive-dst move-in step needed even in this
               fallback. Lane 1 is 0*0+0 = 0.0 computed into the shared
               scratch register and discarded, matching the
               identity-op-identity convention the rest of the vN
               family's two-operand RISC-V fallback already uses (see
               OP_VADD's case) -- one scratch register is enough since
               all three lane-1 operands are the same synthesized
               0.0. fA/fB/fC are cas_expected/result_reg/cas_desired
               respectively, same reuse as OP_FMA/OP_VFMA elsewhere. */
            if (!g_riscv_vadd_warned) {
                g_riscv_vadd_warned = 1;
                fprintf(stderr,
                    "note: 'vadd'/'vsub'/'vmul'/'vdiv'/'vmin'/'vmax'/'vsqrt'/'vabs'/'vneg'/'vdup'/"
                    "'vfma' on --target=riscv are NOT real SIMD -- RISC-V has no fixed-width "
                    "packed-double register (unlike x86-64 SSE2/AArch64 NEON), only the Vector "
                    "extension (RVV), which needs runtime vsetvli setup this compiler does not "
                    "emit. Falling back to two sequential scalar instructions per op (lane 0 = "
                    "your values, lane 1 = the identity op'd with itself for the two-operand "
                    "forms, applied to a synthesized 0.0 for the unary forms, the same broadcast "
                    "value again for vdup, or 0*0+0 for vfma -- matching what an unused upper "
                    "lane always holds on the other two targets: 0.0 for "
                    "add/sub/mul/min/max/sqrt/abs/fma, NaN for div, -0.0 for neg, the broadcast "
                    "value itself for dup) -- correct result, but no parallelism and no speed "
                    "benefit over the plain scalar f-ops on this target.\n");
            }
            {
                const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
                const char *areg = reg__name(TARGET_RISCV, &ins->cas_expected);
                const char *breg = reg__name(TARGET_RISCV, &ins->result_reg);
                const char *creg = reg__name(TARGET_RISCV, &ins->cas_desired);
                const char *fscratch = target_defs[TARGET_RISCV].fscratch;
                /* Lane 0: the real fused multiply-add. */
                fprintf(out, "    fmadd.d %s, %s, %s, %s\n", dstreg, areg, breg, creg);
                /* Lane 1: 0*0+0 = 0.0, discarded. */
                fprintf(out, "    fcvt.d.w %s, zero\n", fscratch);
                fprintf(out, "    fmadd.d %s, %s, %s, %s\n", fscratch, fscratch, fscratch, fscratch);
            }
            break;
        }


        case OP_FCMP:
            /* RISC-V has no single "set flags from float compare"
               instruction the way x86-64/AArch64 do -- flt.d/fle.d/
               feq.d each write a 0/1 integer result directly, no
               separate flags register to read afterward. To still let
               fcmp integrate with the existing OP_JE/JG/JL/... jump
               family unchanged (see OP_FCMP's opcode_t comment), this
               synthesizes an integer "spaceship" result into
               SCRATCH_RV/SCRATCH_RV2 -- the same two registers every
               jump instruction below already reads its comparison
               operands from (see OP_JE and friends) -- so the jump
               that follows an fcmp works exactly as if an integer cmp
               had just run: SCRATCH_RV = (dst > src) ? 1 : (dst < src)
               ? -1 : 0, SCRATCH_RV2 = 0, and an unordered (NaN)
               comparison yields 1 (both flt.d and fle.d report false
               for any NaN operand, so the flt.d-based "less" check
               naturally comes back 0, falling through to the "greater"
               result), matching the "unordered compares as greater"
               convention used on the other two targets. dst.is_f32
               picks the .s (32-bit) forms of flt/feq over the .d
               (64-bit) forms, same width-selection convention as the
               OP_FADD family -- parse-time already guarantees src (if
               a register) is the same width as dst, so no mixed-width
               case can reach here. The integer scratch registers
               SCRATCH_RV/SCRATCH_RV2 and the rest of the spaceship
               sequence below are unaffected by src/dst width, since
               flt/feq always write an integer 0/1 result regardless of
               the float operand width. */
            {
            const char *fcmp_mn = ins->dst.is_f32 ? "s" : "d";
            fprintf(out, "    flt.%s %s, %s, %s\n", fcmp_mn, SCRATCH_RV, reg__name(TARGET_RISCV, &ins->dst), reg__name(TARGET_RISCV, &ins->src));
            fprintf(out, "    feq.%s %s, %s, %s\n", fcmp_mn, SCRATCH_RV2, reg__name(TARGET_RISCV, &ins->dst), reg__name(TARGET_RISCV, &ins->src));
            }
            fprintf(out, "    beqz %s, 1f\n", SCRATCH_RV);
            fprintf(out, "    li %s, -1\n", SCRATCH_RV);
            fprintf(out, "    j 2f\n");
            fprintf(out, "1:\n");
            fprintf(out, "    beqz %s, 3f\n", SCRATCH_RV2);
            fprintf(out, "    li %s, 0\n", SCRATCH_RV);
            fprintf(out, "    j 2f\n");
            fprintf(out, "3:\n");
            fprintf(out, "    li %s, 1\n", SCRATCH_RV);
            fprintf(out, "2:\n");
            fprintf(out, "    li %s, 0\n", SCRATCH_RV2);
            break;

        case OP_FSQRT:
            /* fsqrt.d: direct native instruction, unary (src is the
               whole input -- see the parse-time comment). Negative
               input naturally produces NaN, matching the opcode_t
               comment's contract. dst.is_f32 picks fsqrt.s over
               fsqrt.d, same width-selection convention as the OP_FADD
               family. */
            fprintf(out, "    %s %s, %s\n", ins->dst.is_f32 ? "fsqrt.s" : "fsqrt.d", reg__name(TARGET_RISCV, &ins->dst), reg__name(TARGET_RISCV, &ins->src));
            break;

        case OP_FABS:
            /* fabs.d: direct native instruction (RV64D's own sign-bit-
               clear op) -- like AArch64, no mask-and-and dance needed
               the way x86-64 SSE2 requires. dst.is_f32 picks fabs.s
               over fabs.d, same width-selection convention as the
               OP_FADD family. */
            fprintf(out, "    %s %s, %s\n", ins->dst.is_f32 ? "fabs.s" : "fabs.d", reg__name(TARGET_RISCV, &ins->dst), reg__name(TARGET_RISCV, &ins->src));
            break;

        case OP_FNEG:
            /* fneg.d: direct native instruction (RV64D's own sign-bit-
               flip op) -- same "no mask dance needed" situation as
               fabs just above. dst.is_f32 picks fneg.s over fneg.d,
               same width-selection convention as the OP_FADD family. */
            fprintf(out, "    %s %s, %s\n", ins->dst.is_f32 ? "fneg.s" : "fneg.d", reg__name(TARGET_RISCV, &ins->dst), reg__name(TARGET_RISCV, &ins->src));
            break;

        case OP_FMIN: case OP_FMAX: {
            /* fmin.d/fmax.d: direct native instructions, same
               destructive dst-OP-src shape and immediate-materialization
               path as fadd/etc above (mirrored identically -- see that
               case's comment). Both already implement the "NaN loses to
               a real operand" convention documented on the OP_FMIN/
               OP_FMAX opcode_t comment (RISC-V's fmin.d/fmax.d follow
               IEEE 754-2008 minNum/maxNum semantics, same as the other
               two targets' native instructions). .d -> .s suffix swap
               for f32 (mirrors OP_FADD's own comment about this being
               mnemonic-only on RISC-V); dstreg is unchanged either way
               since sregs==fregs here. Immediate materialization uses
               fmv.w.x + a 32-bit bit pattern for f32 (same as FADD's
               narrowed path), fmv.d.x + the full 64-bit pattern for
               f64. */
            int f32 = ins->dst.is_f32;
            const char *mn = ins->op == OP_FMIN ? (f32 ? "fmin.s" : "fmin.d") : (f32 ? "fmax.s" : "fmax.d");
            const char *dstreg = reg__name(TARGET_RISCV, &ins->dst);
            const char *fscratch = target_defs[TARGET_RISCV].fscratch;
            if (ins->src.kind == OPND_IMM) {
                if (f32) {
                    fprintf(out, "    li %s, 0x%08x\n", SCRATCH_RV, float__bits(ins->src.fimm));
                    fprintf(out, "    fmv.w.x %s, %s\n", fscratch, SCRATCH_RV);
                } else {
                    fprintf(out, "    li %s, 0x%016llx\n", SCRATCH_RV, (unsigned long long)double__bits(ins->src.fimm));
                    fprintf(out, "    fmv.d.x %s, %s\n", fscratch, SCRATCH_RV);
                }
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, fscratch);
            } else {
                fprintf(out, "    %s %s, %s, %s\n", mn, dstreg, dstreg, reg__name(TARGET_RISCV, &ins->src));
            }
            break;
        }

        case OP_FMA:
            /* fmadd.d fDST, fA, fB, fC computes fDST = fA*fB + fC
               directly (RISC-V's fused multiply-add, like AArch64's
               fmadd, takes all three source registers as distinct
               operands -- no pre-move into dst needed). fA/fB/fC are
               cas_expected/result_reg/cas_desired respectively -- see
               the instr_t field comments and the OP_FMA opcode_t
               comment for why those fields are reused. dst.is_f32
               picks fmadd.s over fmadd.d, same width-selection
               convention as the OP_FADD family. */
            fprintf(out, "    %s %s, %s, %s, %s\n", ins->dst.is_f32 ? "fmadd.s" : "fmadd.d", reg__name(TARGET_RISCV, &ins->dst),
                    reg__name(TARGET_RISCV, &ins->cas_expected), reg__name(TARGET_RISCV, &ins->result_reg),
                    reg__name(TARGET_RISCV, &ins->cas_desired));
            break;

        case OP_I2F:
            /* dst.is_f32 picks fcvt.s.l over fcvt.d.l, same
               width-selection convention as the OP_FADD family --
               src is always the 64-bit integer register file (Chard's
               integer registers are always 64-bit, matching how
               FCVT.S.L/FCVT.L.S are RV64-only forms with no 32-bit
               integer-register variant to choose between), so only
               dst's float width varies here. */
            if (ins->src.kind == OPND_IMM) {
                fprintf(out, "    li %s, %ld\n", SCRATCH_RV, ins->src.imm);
                fprintf(out, "    %s %s, %s\n", ins->dst.is_f32 ? "fcvt.s.l" : "fcvt.d.l", reg__name(TARGET_RISCV, &ins->dst), SCRATCH_RV);
            } else {
                fprintf(out, "    %s %s, %s\n", ins->dst.is_f32 ? "fcvt.s.l" : "fcvt.d.l", reg__name(TARGET_RISCV, &ins->dst), reg__name(TARGET_RISCV, &ins->src));
            }
            break;

        case OP_F2I:
            /* fcvt.l.d ..., rtz: RTZ (round-toward-zero) rounding mode
               explicitly, matching the opcode_t comment's "truncating
               toward zero" contract (the instruction's default dynamic
               rounding mode would otherwise follow whatever fcsr is
               currently set to). src.is_f32 picks fcvt.l.s over
               fcvt.l.d here, since F2I's float operand is src (the
               mirror image of I2F, whose float operand is dst) -- the
               rtz suffix is unaffected by width and stays on both
               forms. */
            fprintf(out, "    %s %s, %s, rtz\n", ins->src.is_f32 ? "fcvt.l.s" : "fcvt.l.d", reg__name(TARGET_RISCV, &ins->dst), reg__name(TARGET_RISCV, &ins->src));
            break;

        case OP_SYSCALL: {
            /* Linux RISC-V syscall ABI: number in a7, args in a0-a6.
               Only the args the user actually wrote are emitted. */
            static const char *argregs[6] = {"a0", "a1", "a2", "a3", "a4", "a5"};
            for (int a = 1; a < ins->nargs; a++) {
                /* '&SYM' (is_addr_of): materialize SYM's address directly
                   into the arg register via 'la', same as OP_LEA's
                   non-local case, instead of the value-load
                   emit_riscv_load_scratch would otherwise perform. Must
                   be checked before the is_mem_operand branch below --
                   see the matching x86-64 comment for why.
                   check_addr_of_violations() guarantees is_addr_of can't
                   reach here except in this exact slot. */
                if (ins->args[a].kind == OPND_SYM && ins->args[a].is_addr_of) {
                    fprintf(out, "    la %s, %s\n", argregs[a - 1], ins->args[a].sym);
                    continue;
                }
                if (ins->args[a].kind == OPND_LOCAL && ins->args[a].is_addr_of) {
                    /* '&local' (is_addr_of on OPND_LOCAL): same idea as
                       the OPND_SYM branch above, just s0-relative instead
                       of 'la'. riscv_local_base already emits the
                       frames_up saved-s0 chase (if any) into the arg
                       register itself (reused as scratch, same pattern
                       emit_riscv_load_scratch uses), then 'addi' applies
                       -local_offset to land the final address in that
                       same register. */
                    const char *r = argregs[a - 1];
                    const char *base = riscv_local_base(out, &ins->args[a], r);
                    riscv_emit_local_addr(out, r, base, -(long)ins->args[a].local_offset);
                    continue;
                }
                if (is_mem_operand(ins->args[a].kind)) {
                    emit_riscv_load_scratch(out, &ins->args[a], SCRATCH_RV);
                    fprintf(out, "    mv %s, %s\n", argregs[a - 1], SCRATCH_RV);
                } else {
                    emit_riscv_mov_operand(out, argregs[a - 1], &ins->args[a]);
                }
            }
            if (is_mem_operand(ins->args[0].kind)) {
                emit_riscv_load_scratch(out, &ins->args[0], SCRATCH_RV);
                fprintf(out, "    mv a7, %s\n", SCRATCH_RV);
            } else {
                emit_riscv_mov_operand(out, "a7", &ins->args[0]);
            }
            fprintf(out, "    ecall\n");
            break;
        }

        case OP_READ: case OP_WRITE: {
            /* Named wrappers around read(2)/write(2): same 3-register
               argument shape as OP_SYSCALL (fd, buf, len -> a0, a1,
               a2), just with the syscall number already chosen instead
               of taken from args[0]. Linux RISC-V: read = 63,
               write = 64 (matching the number OP_STDOUT already uses).

               buf (arg 1) is always treated as an address, same
               reasoning as the other two backends: a symbol/local
               names the buffer, so it needs its address (mirroring
               OP_LEA's own local-vs-symbol branching below: 'addi
               reg, sp, off' for a local, 'la reg, sym' for a symbol --
               'la' already computes an address, unlike 'lb'/'lw'/etc
               which load a value), not a value load. A register
               operand passes through unchanged. */
            static const char *argregs3[3] = {"a0", "a1", "a2"};
            for (int a = 0; a < 3; a++) {
                if (a == 1 && ins->args[a].kind == OPND_LOCAL) {
                    const char *base = riscv_local_base(out, &ins->args[a], argregs3[a]);
                    riscv_emit_local_addr(out, argregs3[a], base, -(long)ins->args[a].local_offset);
                    continue;
                }
                if (a == 1 && ins->args[a].kind == OPND_SYM) {
                    fprintf(out, "    la %s, %s\n", argregs3[a], ins->args[a].sym);
                    continue;
                }
                if (is_mem_operand(ins->args[a].kind)) {
                    emit_riscv_load_scratch(out, &ins->args[a], SCRATCH_RV);
                    fprintf(out, "    mv %s, %s\n", argregs3[a], SCRATCH_RV);
                } else {
                    emit_riscv_mov_operand(out, argregs3[a], &ins->args[a]);
                }
            }
            fprintf(out, "    li a7, %d\n", ins->op == OP_READ ? 63 : 64);
            fprintf(out, "    ecall\n");
            break;
        }

        case OP_LIBC_INIT:
            /* No code needed: the RISC-V C ABI's own crt startup
               already ran before 'main' (the retargeted entry label --
               see apply_entry_symbol_override) is reached. Same story as
               the other two backends. */
            break;

        case OP_LIBC_CALL: {
            /* RISC-V calling convention: integer/pointer args in
               a0-a7 in order; Chard caps libcall at MAX_LIBC_ARGS (6),
               so only a0-a5 are ever used here. No vector-argument
               register to set for variadics, same as AArch64. Stack
               alignment (sp % 16 == 0 at 'call') holds for the same
               reasons noted in the x86-64 OP_LIBC_CALL comment. */
            static const char *libc_argregs[MAX_LIBC_ARGS] = {"a0", "a1", "a2", "a3", "a4", "a5"};
            for (int a = 0; a < ins->nargs; a++) {
                if (ins->args[a].kind == OPND_SYM && ins->args[a].is_addr_of) {
                    fprintf(out, "    la %s, %s\n", libc_argregs[a], ins->args[a].sym);
                    continue;
                }
                if (ins->args[a].kind == OPND_LOCAL && ins->args[a].is_addr_of) {
                    const char *r = libc_argregs[a];
                    const char *base = riscv_local_base(out, &ins->args[a], r);
                    riscv_emit_local_addr(out, r, base, -(long)ins->args[a].local_offset);
                    continue;
                }
                if (is_mem_operand(ins->args[a].kind)) {
                    emit_riscv_load_scratch(out, &ins->args[a], SCRATCH_RV);
                    fprintf(out, "    mv %s, %s\n", libc_argregs[a], SCRATCH_RV);
                } else {
                    emit_riscv_mov_operand(out, libc_argregs[a], &ins->args[a]);
                }
            }
            fprintf(out, "    call %s\n", ins->dst.sym);
            if (ins->dst.kind == OPND_REG) {
                /* Return value comes back in a0; move it into the
                   requested r1-r12 slot unless it's already a0
                   (reg_num 1 -- see target_defs[TARGET_RISCV].regs). */
                if (ins->dst.reg_num != 1)
                    fprintf(out, "    mv %s, a0\n", target_defs[TARGET_RISCV].regs[ins->dst.reg_num]);
            }
            break;
        }
        default:
            /* See the matching x86-64 default: case for why this
               exists -- same safety net, same reasoning. */
            g_source_line = NULL;
            fail_fmt("internal error: chard RISC-V backend: unhandled opcode %d", (int)ins->op);
        }
    }
}

