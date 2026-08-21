/* riscv.h -- auto-generated declarations for the 'backend_riscv' module
 * (7 function(s), defined in riscv.c). */
#ifndef CHARD_MOD_RISCV_H
#define CHARD_MOD_RISCV_H

#include "../../chard_types.h"
#include "../../chard_globals.h"

const char *riscv_local_base(FILE *out, const operand_t *o, const char *scratch);
const char *riscv_safe_offset(FILE *out, const char *base, long offset,
                                      const char *scratch, long *out_offset);
void emit_riscv_load_scratch(FILE *out, const operand_t *o, const char *scratch);
void riscv_emit_local_addr(FILE *out, const char *dst, const char *base, long magnitude);
void emit_riscv_mov_operand(FILE *out, const char *dstreg, const operand_t *o);
void riscv_emit_int_val(FILE *out, long val, const int *is_label, char *const *val_labels, int v, int first);
void emit__riscv(FILE *out);

#endif /* CHARD_MOD_RISCV_H */
