/* symtab.h -- auto-generated declarations for the 'equs_aliases_enums_structs' module
 * (12 function(s), defined in symtab.c). */
#ifndef CHARD_MOD_SYMTAB_H
#define CHARD_MOD_SYMTAB_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

equ_def_t *find__equ(const char *name);
alias_def_t *find__alias(const char *name);
enum_member_t *find_enum_member(const char *qualified_name);
int enum_name_already_declared(const char *enum_name);
struct_def_t *find_struct_def(const char *name);
struct_field_t *find_struct_field(struct_def_t *sd, const char *field_name);
void enum_finish_member(enum_collect_t *ec, const char *member_text);
void struct_finish_field(struct_def_t *sd, const char *field_text);
void substitute_all_equs(void);
void substitute_all_aliases(void);
void substitute_all_enums(void);
void substitute_all_sizeofs(void);

#endif /* CHARD_MOD_SYMTAB_H */
