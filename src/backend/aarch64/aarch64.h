/* aarch64.h -- auto-generated declarations for the 'backend_aarch64' module
 * (7 function(s), defined in aarch64.c). */
#ifndef CHARD_MOD_AARCH64_H
#define CHARD_MOD_AARCH64_H

#include "../../chard_types.h"
#include "../../chard_globals.h"

const char *aarch64_local_base(FILE *out, const operand_t *o);
const char *aarch64_safe_offset(FILE *out, const char *base, long offset,
                                        int sz, const char *scratch, long *out_offset);
void emit_aarch64_load_scratch(FILE *out, const operand_t *o);
void aarch64_emit_local_addr(FILE *out, const char *dst, const char *base, int magnitude);
const char *emit_aarch64_addr_into_scratch(FILE *out, const operand_t *o);
void arm_emit_int_val(FILE *out, long val, const int *is_label, char *const *val_labels, int v, int first);
void emit__aarch64(FILE *out);

#endif /* CHARD_MOD_AARCH64_H */
