/* pins.h -- auto-generated declarations for the 'pins' module
 * (12 function(s), defined in pins.c). */
#ifndef CHARD_MOD_PINS_H
#define CHARD_MOD_PINS_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

global_pin_t *find_pin_by_reg(int reg_num);
global_pin_t *find_pin_by_sym(const char *sym);
int find_param_reg(const char *name);
int parse_global_pin(char *tokens[], int ntok, char *raw_line);
void splice_global_pin_loads(void);
int operand_hits_pin(const operand_t *o, global_pin_t **out_pin);
void fail_pin_violation(const instr_t *ins, global_pin_t *p);
void check_addr_of_operand(const operand_t *o, const char *where);
void check_addr_of_violations(void);
void check_argv_pin_collision(void);
void check_init_scratch_collision(void);
void check_global_pin_violations(void);

#endif /* CHARD_MOD_PINS_H */
