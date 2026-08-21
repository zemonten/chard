#include "../../../chard.h"

int parse__register(const char *tok, operand_t *out) {
    if (strcmp(tok, "sp") == 0) {
        out->kind = OPND_REG;
        out->is_sp = 1;
        out->is_float = 0;
        out->is_f32 = 0;
        out->reg_num = 0;
        return 1;
    }
    if (tok[0] == 'r' && isdigit((unsigned char)tok[1])) {
        int n = atoi(tok + 1);
        if (n >= 1 && n <= 12) {
            out->kind = OPND_REG;
            out->is_sp = 0;
            out->is_float = 0;
            out->is_f32 = 0;
            out->reg_num = n;
            return 1;
        }
    }
    /* Float registers: f1-f8, a completely separate namespace from
       r1-r12 (see the operand_t.is_float comment). 8 rather than 12 --
       matching how many of each target's own float/SIMD register file
       Chard reserves for float-register-argument purposes (see
       target_def_t.fregs below) without also having to carve out a
       dedicated float scratch register the way SCRATCH_X86/etc do for
       integers; f8 is never assigned to the user, so codegen can
       always borrow it as scratch the same way it borrows r14/x12/t0
       for integers. */
    if (tok[0] == 'f' && isdigit((unsigned char)tok[1])) {
        int n = atoi(tok + 1);
        if (n >= 1 && n <= 7) {
            out->kind = OPND_REG;
            out->is_sp = 0;
            out->is_float = 1;
            out->is_f32 = 0;
            out->reg_num = n;
            return 1;
        }
    }
    /* Single-precision float registers: s1-s8, a third namespace
       alongside r1-r12 and f1-f8 rather than a width tag on f1-f8 (see
       the operand_t.is_f32 comment for why). Same f8-is-scratch
       pattern as the f1-f8 file above: s8 is reserved as the f32
       scratch register and never assigned to the user, so codegen can
       always borrow it the same way it borrows f8 for f64 scratch
       work. */
    if (tok[0] == 's' && isdigit((unsigned char)tok[1])) {
        int n = atoi(tok + 1);
        if (n >= 1 && n <= 7) {
            out->kind = OPND_REG;
            out->is_sp = 0;
            out->is_float = 1;
            out->is_f32 = 1;
            out->reg_num = n;
            return 1;
        }
    }
    return 0;
}

const char *reg__name(target_t t, operand_t *o) {
    if (o->is_sp) return target_defs[t].sp;
    if (o->is_float) return o->is_f32 ? target_defs[t].sregs[o->reg_num] : target_defs[t].fregs[o->reg_num];
    return target_defs[t].regs[o->reg_num];
}

const char *width_reg_name(target_t t, operand_t *o, int size_bytes) {
    const target_def_t *d = &target_defs[t];
    const char *const *table = size_bytes == 1 ? d->regs_8 :
                                size_bytes == 2 ? d->regs_16 :
                                size_bytes == 4 ? d->regs_32 : NULL;
    if (!table) return d->regs[o->reg_num];
    return table[o->reg_num];
}

