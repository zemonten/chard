#include "../../../chard.h"

int in_local_frame(void) { return local_frame_depth > 0; }

local_frame_t *current_local_frame(void) {
    return in_local_frame() ? &local_frame_stack[local_frame_depth - 1] : NULL;
}

decl_t *declare__local(const char *name, int size_bytes) {
    if (!in_local_frame())
        fail("'local' may only be declared inside a @label { ... } block");
    if (find__decl(name)) failf("redeclaration of '%s'", name);
    DA_ENSURE(decls, decls_cap, ndecls, decl_t);

    local_frame_t *f = current_local_frame();
    int align = size_bytes; /* 1/2/4/8, all powers of two */
    int candidate = f->frame_size + size_bytes;
    int off = (candidate + (align - 1)) & ~(align - 1);

    decl_t *d = &decls[ndecls++];
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, MAX_SYMLEN - 1);
    d->section = SEC_LOCAL;
    d->size_bytes = size_bytes;
    d->local_offset = off; /* distance below fp: real address is [fp - off] */
    d->local_depth = local_frame_depth; /* current nesting depth, 1-based */

    f->frame_size = off;
    return d;
}

decl_t *declare_local_array(const char *name, int size_bytes, int array_len) {
    if (!in_local_frame())
        fail("'local' may only be declared inside a @label { ... } block");
    if (find__decl(name)) failf("redeclaration of '%s'", name);
    DA_ENSURE(decls, decls_cap, ndecls, decl_t);
    if (array_len <= 0) fail("array size must be a positive integer (local iK name[N];)");

    local_frame_t *f = current_local_frame();
    int align = size_bytes; /* 1/2/4/8, all powers of two */
    long total = (long)size_bytes * (long)array_len;
    if (total > 1 << 20) fail("local array too large (over 1 MiB) -- did you mean a smaller size, or 'bss' for something this big?");
    long candidate = (long)f->frame_size + total;
    long off = (candidate + (align - 1)) & ~((long)align - 1);

    decl_t *d = &decls[ndecls++];
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, MAX_SYMLEN - 1);
    d->section = SEC_LOCAL;
    d->size_bytes = size_bytes;
    d->array_len = array_len;
    d->local_offset = (int)off; /* element 0's offset -- the array's base */
    d->local_depth = local_frame_depth;

    f->frame_size = (int)off;
    return d;
}

decl_t *declare_local_struct(const char *name, struct_def_t *sd) {
    if (sd->nfields == 0) failf("struct '%s' has no fields (an empty struct cannot be instantiated)", sd->name);
    decl_t *d = declare_local_array(name, 1, sd->total_size);
    strncpy(d->struct_type_name, sd->name, MAX_SYMLEN - 1);
    d->struct_type_name[MAX_SYMLEN - 1] = '\0';
    return d;
}

void open_local_frame(void) {
    if (local_frame_depth >= MAX_LOCAL_FRAME_DEPTH) fail("@label blocks nested too deep for locals");
    local_frame_t *f = &local_frame_stack[local_frame_depth++];
    /* local_frame_stack slots are reused across unrelated blocks as depth
       goes up and back down, so the previous occupant's heap-allocated
       spill_* arrays (see local_frame_t) must be freed before this slot
       is zeroed and handed to a new block, or they leak every time a
       block at this depth closes and a sibling/later block opens. */
    free(f->spill_is_local);
    free(f->spill_regs);
    free(f->spill_local_decl_idx);
    free(f->spill_decl_idx);
    memset(f, 0, sizeof(*f)); /* local_frame_stack slots are reused across
                                  unrelated blocks as depth goes up and back
                                  down, so every field -- not just the two
                                  this function sets below -- must start
                                  clean, or a previous occupant's
                                  is_function_root/func_sig (see
                                  close_local_frame) leaks into a block
                                  that was never declared with parameters */
    f->frame_size = 0;
    f->first_decl_idx = ndecls;

    instr_t open; memset(&open, 0, sizeof(open));
    open.op = OP_FRAME_OPEN;
    open.frame_bytes = 0; /* patched below once the block's '}' is seen */
    f->prologue_instr_idx = nprog;
    push__instr(open);
}

void note_written_register(const operand_t *d, int *out, int *out_isfloat, int *out_isf32, int *n) {
    if (d->kind != OPND_REG || d->is_sp) return;
    for (int j = 0; j < *n; j++) if (out[j] == d->reg_num && out_isfloat[j] == d->is_float && out_isf32[j] == d->is_f32) return;
    if (*n >= MAX_PARAMS) fail("internal error: more distinct written registers than r1-r12 allows");
    out_isfloat[*n] = d->is_float;
    out_isf32[*n] = d->is_f32;
    out[(*n)++] = d->reg_num;
}

int scan_written_registers(int from, int to, int *out, int *out_isfloat, int *out_isf32) {
    int n = 0;
    for (int idx = from; idx < to; idx++) {
        instr_t *ins = &prog[idx];
        switch (ins->op) {
            case OP_S2I:
            case OP_ATOMIC_ADD: case OP_ATOMIC_SUB: case OP_ATOMIC_AND:
            case OP_ATOMIC_OR: case OP_ATOMIC_XOR: case OP_ATOMIC_SWAP:
            case OP_ATOMIC_CAS:
                /* dst is a read-only input (rBUF, or the memory
                   location) here, not a write -- only result_reg is. */
                note_written_register(&ins->result_reg, out, out_isfloat, out_isf32, &n);
                break;
            case OP_I2S:
                /* Both dst (rBUF) and len_reg (rLEN) are writes. */
                note_written_register(&ins->dst, out, out_isfloat, out_isf32, &n);
                note_written_register(&ins->len_reg, out, out_isfloat, out_isf32, &n);
                break;
            case OP_BCOPY:
                /* Unlike OP_STORE (whose dst is OPND_SYM/OPND_LOCAL, so
                   note_written_register's own OPND_REG guard already
                   makes it a silent no-op under `default`), bcopyN's
                   dst genuinely IS a register -- but it holds a pointer
                   *value* being read and dereferenced, not a value
                   being overwritten (mirrors OP_ISTORE's base_reg/
                   idx_reg, which are likewise registers holding
                   read-only pointer/index values). Needs its own
                   explicit no-op case here, or `default` below would
                   wrongly count it as a write and give it (harmless
                   but incorrect) callee-saved protection it doesn't
                   need. */
                break;
            case OP_JMP: case OP_CALL:
                /* dst is normally OPND_LABEL (a fixed jump/call target,
                   which note_written_register's OPND_REG guard already
                   no-ops on under `default`) but can now also be
                   OPND_REG for an indirect jmp/call rN -- same shape as
                   OP_BCOPY above: the register holds an address being
                   read and jumped through, not a value being
                   overwritten, so this needs its own explicit no-op
                   case rather than falling into `default`. */
                break;
            default:
                note_written_register(&ins->dst, out, out_isfloat, out_isf32, &n);
                break;
        }
    }
    /* simple insertion sort -- n is at most 12, and a stable, readable
       register order in the emitted save/restore sequence is worth
       more here than using a library sort for such a small list.
       out_isfloat/out_isf32 must be carried along with each swap so
       they stay aligned with out[]. */
    for (int i = 1; i < n; i++) {
        int key = out[i], keyf = out_isfloat[i], keyf32 = out_isf32[i], j = i - 1;
        while (j >= 0 && out[j] > key) { out[j + 1] = out[j]; out_isfloat[j + 1] = out_isfloat[j]; out_isf32[j + 1] = out_isf32[j]; j--; }
        out[j + 1] = key;
        out_isfloat[j + 1] = keyf;
        out_isf32[j + 1] = keyf32;
    }
    return n;
}

int relocate_frame_close_before_rets(int frame_open_idx, int frame_close_idx, int frame_bytes,
                                             int *out_ends_in_ret, int **out_ret_idxs, int *out_nrets) {
    int *ret_idxs = NULL;
    int ret_idxs_cap = 0;
    int nrets = 0;
    for (int idx = frame_open_idx + 1; idx < frame_close_idx; idx++) {
        if (prog[idx].op == OP_RET) {
            DA_ENSURE(ret_idxs, ret_idxs_cap, nrets, int);
            ret_idxs[nrets++] = idx;
        }
    }

    int ends_in_ret = (frame_close_idx > frame_open_idx + 1 && prog[frame_close_idx - 1].op == OP_RET);

    instr_t frame_close_seq[1];
    memset(&frame_close_seq[0], 0, sizeof(instr_t));
    frame_close_seq[0].op = OP_FRAME_CLOSE;
    frame_close_seq[0].frame_bytes = frame_bytes;

    /* Insert back-to-front (highest ret_idxs[] first) so earlier
       insertions don't shift the indices later ones still need to
       target; each insertion also shifts frame_close_idx's real
       position forward (every ret_idxs[i] is < frame_close_idx by
       construction), so that drift has to be tracked and applied
       before frame_close_idx is used again below. Every ret_idxs[i]
       shifts forward by the same running total too, so they're
       corrected in the same pass for the caller's benefit. */
    int shift = 0;
    for (int i = nrets - 1; i >= 0; i--) {
        insert_instrs_at(ret_idxs[i], frame_close_seq, 1);
        shift += 1;
    }
    frame_close_idx += shift;
    /* Recompute each ret_idxs[i]'s current position: everything at or
       after a given original ret_idxs[i] shifted forward by 1 for
       every OTHER ret_idxs[j] with j <= i that was already inserted
       before it in the loop above (i.e. every ret at or before it in
       program order, since insertion went highest-index-first). The
       ret itself also moved forward by 1 (its own frame-close landed
       immediately before it), so ret_idxs[i]'s new position is its
       original index plus (i+1). */
    for (int i = 0; i < nrets; i++) ret_idxs[i] += (i + 1);

    if (ends_in_ret) {
        delete_instr_at(frame_close_idx);
    }
    /* else: the trailing OP_FRAME_CLOSE close_local_frame() appended
       stays exactly where it is -- it's this block's real epilogue for
       the fall-off-the-end path, and frame_close_idx (as returned)
       still names it correctly. */

    if (out_ends_in_ret) *out_ends_in_ret = ends_in_ret;
    /* Hand ownership of ret_idxs (heap-allocated above via DA_ENSURE)
       straight to the caller instead of memcpy-ing into a
       caller-supplied fixed-size buffer -- the caller doesn't know
       nrets in advance (finding that out is this function's whole
       job), so it can no longer pre-size a buffer itself. The caller
       is responsible for free()-ing it once done (see call site). */
    if (out_ret_idxs) *out_ret_idxs = ret_idxs; else free(ret_idxs);
    if (out_nrets) *out_nrets = nrets;
    return frame_close_idx;
}

void wrap_function_body(int frame_open_idx, int frame_close_idx, int nparams, int ret_reg,
                                int ends_in_ret, const int *ret_idxs, int nrets) {
    int written[MAX_PARAMS];
    int written_isfloat[MAX_PARAMS];
    int written_isf32[MAX_PARAMS];
    int nwritten = scan_written_registers(frame_open_idx + 1, frame_close_idx, written, written_isfloat, written_isf32);

    /* Float registers (f1-f7/s1-s7) are never bound to function
       parameters or return values -- those bindings are all integer-
       register concepts (nparams/ret_reg compare against the integer
       r1-r12 numbering) -- so a written float register (f64 or f32
       alike) always needs protecting; only integer registers can be a
       parameter or the return register and thus get skipped below. */
    int nsave = 0;
    int to_save[MAX_PARAMS];
    int to_save_isfloat[MAX_PARAMS];
    int to_save_isf32[MAX_PARAMS];
    for (int i = 0; i < nwritten; i++) {
        if (!written_isfloat[i] && written[i] <= nparams) continue;   /* parameter register: caller-relinquished input */
        if (!written_isfloat[i] && written[i] == ret_reg) continue;    /* return register: the point is to change it */
        to_save_isfloat[nsave] = written_isfloat[i];
        to_save_isf32[nsave] = written_isf32[i];
        to_save[nsave++] = written[i];
    }
    if (nsave == 0) return; /* nothing this function touches needs protecting */

    instr_t restore_seq[MAX_PARAMS];
    for (int i = 0; i < nsave; i++) {
        memset(&restore_seq[i], 0, sizeof(instr_t));
        restore_seq[i].op = OP_POP;
        restore_seq[i].dst.kind = OPND_REG;
        restore_seq[i].dst.reg_num = to_save[nsave - 1 - i]; /* pop in reverse push order */
        restore_seq[i].dst.is_float = to_save_isfloat[nsave - 1 - i];
        restore_seq[i].dst.is_f32 = to_save_isf32[nsave - 1 - i];
        restore_seq[i].is_callee_save_push = 1; /* see instr_t's comment: distinguishes this from a user 'pop;' for CFI */
    }

    /* ret_idxs[i] currently points at the RET instruction itself (see
       relocate_frame_close_before_rets's recompute pass); its already-
       relocated frame-close sits immediately before it, at index
       ret_idxs[i]-1. The callee-saved registers here were PUSHED after
       OP_FRAME_OPEN's 'sub rsp, N' ran (see save_seq's insertion point
       below: frame_open_idx + 1) -- so on the stack they sit BELOW the
       frame's own local-variable space, closer to the bottom. The
       frame-close's 'mov rsp, rbp' resets rsp in one jump back to the
       frame's base, skipping over that pushed region entirely; a 'pop'
       for one of these registers must run BEFORE that reset (i.e.
       inserted at ret_idxs[i]-1, pushing the frame-close and ret both
       one further step forward), or it reads whatever now happens to
       sit at the (wrong, already-reset) stack position instead of the
       value that was actually pushed -- and worse, leaves rsp
       misaligned for 'ret' itself to pop the wrong return address. */
    for (int i = nrets - 1; i >= 0; i--)
        insert_instrs_at(ret_idxs[i] - 1, restore_seq, nsave);

    /* Fall-off-the-end case (no explicit trailing ret;): the frame-
       close close_local_frame() appended is still sitting at its
       original frame_close_idx (relocate_frame_close_before_rets only
       moves/deletes it for the ends_in_ret case). Same reasoning as
       above -- the pops must come before it, not after. */
    if (!ends_in_ret && nsave > 0) insert_instrs_at(frame_close_idx, restore_seq, nsave);

    instr_t save_seq[MAX_PARAMS];
    for (int i = 0; i < nsave; i++) {
        memset(&save_seq[i], 0, sizeof(instr_t));
        save_seq[i].op = OP_PUSH;
        save_seq[i].src.kind = OPND_REG;
        save_seq[i].src.reg_num = to_save[i];
        save_seq[i].src.is_float = to_save_isfloat[i];
        save_seq[i].src.is_f32 = to_save_isf32[i];
        save_seq[i].is_callee_save_push = 1; /* see instr_t's comment: distinguishes this from a user 'push;' for CFI */
    }
    insert_instrs_at(frame_open_idx + 1, save_seq, nsave);
}

void close_local_frame(void) {
    local_frame_t *top = &local_frame_stack[local_frame_depth - 1];
    if (top->nspilled > 0) {
        int last = top->nspilled - 1;
        if (top->spill_is_local[last]) {
            decl_t *d = &decls[top->spill_local_decl_idx[last]];
            failf("'}' with an outstanding 'spill' ('%s') never matched by 'unspill' in this block", d->name);
        } else {
            char regbuf[16];
            snprintf(regbuf, sizeof(regbuf), "r%d", top->spill_regs[last]);
            failf("'}' with an outstanding 'spill' (%s) never matched by 'unspill' in this block", regbuf);
        }
    }

    local_frame_t f = local_frame_stack[--local_frame_depth];
    prog[f.prologue_instr_idx].frame_bytes = f.frame_size;

    /* If this pop just closed @main's own frame (or dropped below it --
       can't happen in practice since @main can't nest inside anything
       else, but checking '<' rather than '==' costs nothing and avoids
       relying on that invariant here), extern's lexical "inside @main"
       window is over. See g_in_main_block's declaration. */
    if (g_in_main_block && local_frame_depth < g_main_block_frame_depth) {
        g_in_main_block = 0;
        g_main_block_frame_depth = -1;
    }

    instr_t close; memset(&close, 0, sizeof(close));
    close.op = OP_FRAME_CLOSE;
    close.frame_bytes = f.frame_size;
    int frame_close_idx = nprog;
    push__instr(close);

    /* CFI needs to know, from the function's own OP_LABEL, where its
       matching OP_FRAME_CLOSE landed -- .cfi_endproc on GAS targets goes
       right after it, and x86-64's hand-built .eh_frame FDE needs the
       byte span between the two. Same back-patch shape as frame_bytes
       above: recorded now because frame_close_idx isn't known until
       here, applied onto the label instruction pushed back when this
       block was opened. */
    if (f.is_function_root) prog[f.label_instr_idx].func_frame_close_idx = frame_close_idx;

    ndecls = f.first_decl_idx;

    /* Any block with locals needs its frame torn down before it
       returns, whether or not it's a '@name(...) -> rN: { }' function
       with a declared signature -- a plain '@name: { ... ret; }' block
       used as a callable subroutine (via 'call @name;') has the same
       stack-discipline requirement. relocate_frame_close_before_rets
       runs unconditionally for exactly that reason. */
    int ends_in_ret, nrets;
    int *ret_idxs = NULL;
    frame_close_idx = relocate_frame_close_before_rets(f.prologue_instr_idx, frame_close_idx, f.frame_size,
                                                        &ends_in_ret, &ret_idxs, &nrets);

    /* Explicit-control-flow rule: EVERY block that opened a local frame
       via open_local_frame() -- i.e. any '@name: { ... }' plain
       callable subroutine (call @name;) or '@name(...) -> rN { ... }'
       real function -- must end its last statement in an explicit
       'ret;'. No exceptions for void vs value-returning, and no
       exceptions for a plain subroutine vs a signature'd function:
       both reach this same close_local_frame() and both went through
       relocate_frame_close_before_rets's "must pop saved registers and
       tear down the frame before every ret" handling identically (see
       that function's own comment for why the two are treated the
       same there already). Chard used to silently synthesize a
       trailing ret at '}' for whichever body fell off the end without
       one; that's exactly the kind of hidden control-flow decision the
       language is moving away from -- a function you meant to give a
       value in 'rN' but forgot both the '-> rN' *and* the final
       'ret;' used to compile clean; now the missing 'ret;' surfaces
       immediately, at the one place ('}') that always knows whether
       the body actually ended in one.
       This does NOT affect 'if'/'while'/'for' blocks, which never call
       open_local_frame() themselves (only a '@name'-declared body
       does; see open_local_frame's sole call site), so they never
       reach close_local_frame() at all and are unconditionally exempt.

       ONE exception to "must end in ret": a body whose last statement
       is 'exit(N);'. exit() is the process-terminating syscall wrapper
       (see OP_EXIT) -- it genuinely never returns control to its
       caller, so a 'ret;' placed after it is not "the explicit
       statement of how this path ends," it's unreachable dead code
       that could never execute regardless of whether it's written.
       Requiring it wouldn't make the program more explicit, it would
       just make every libc-linked @main/@start that terminates via
       exit() carry a line that exists purely to satisfy the parser.
       This is why the check below is "ends in ret OR ends in exit",
       not just "ends in ret" -- exit() is the one other statement
       shape that unambiguously describes its own control-flow ending,
       the same way ret does, without Chard having to guess. */
    int ends_in_exit = (frame_close_idx > f.prologue_instr_idx + 1 && prog[frame_close_idx - 1].op == OP_EXIT);
    /* Same exemption, same reasoning, for BARE mode's 'halt;' -- see
       OP_HALT's declaration. exit() and halt are the two statement
       shapes (one per mode) that unambiguously end their own
       control-flow without a ret, so both are checked here. */
    int ends_in_halt = (frame_close_idx > f.prologue_instr_idx + 1 && prog[frame_close_idx - 1].op == OP_HALT);
    if (!ends_in_ret && !ends_in_exit && !ends_in_halt) {
        const char *fname = f.func_sig ? f.func_sig->name : NULL;
        if (fname) {
            failf("function body for '%s' falls off the end without an explicit 'ret;' (or a "
                  "terminal 'exit(N);'/'halt;') -- every function must end its last statement in 'ret;', "
                  "even one with no return value (Chard no longer inserts a ret for you)", fname);
        } else {
            fail("subroutine body falls off the end without an explicit 'ret;' (or a terminal "
                 "'exit(N);'/'halt;') -- every '@name: { ... }' block callable via 'call' must end its "
                 "last statement in 'ret;' (Chard no longer inserts a ret for you)");
        }
    }

    if (f.is_function_root) {
        in_function = 0;
        memset(&current_params, 0, sizeof(current_params));
        /* f.func_sig is NULL for a plain '@name: { ... }' subroutine with
           no '(params) -> rN' declaration -- it binds no parameter
           registers and no return register, so nparams=0/ret_reg=0 (0 is
           a safe sentinel: real registers are numbered 1-12) correctly
           tells scan_written_registers/wrap_function_body "nothing here
           is exempt from callee-save protection," rather than skipping
           the wrap entirely as it used to. */
        int wp_nparams = f.func_sig ? f.func_sig->nparams : 0;
        int wp_ret_reg = f.func_sig ? f.func_sig->ret_reg : 0;
        wrap_function_body(f.prologue_instr_idx, frame_close_idx, wp_nparams, wp_ret_reg,
                            ends_in_ret, ret_idxs, nrets);
    }

    free(ret_idxs);
}

