/* regs.h -- auto-generated declarations for the 'registers' module
 * (3 function(s), defined in regs.c). */
#ifndef CHARD_MOD_REGS_H
#define CHARD_MOD_REGS_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

int parse__register(const char *tok, operand_t *out);
const char *reg__name(target_t t, operand_t *o);
const char *width_reg_name(target_t t, operand_t *o, int size_bytes);

#endif /* CHARD_MOD_REGS_H */
