/* chard_globals.h -- auto-generated: extern declarations for every
 * shared mutable global that used to be `static` file-scope state in
 * the single-file chard.c. Defined once in chard_globals.c. */
#ifndef CHARD_GLOBALS_H
#define CHARD_GLOBALS_H

#include "chard_types.h"

extern long g_heap_size_bytes;
extern int g_init_scratch_reg;
extern int g_finit_scratch_reg;
extern out_mode_t g_mode;
extern int g_mode_seen;
extern decl_t *decls;
extern int ndecls;
extern int decls_cap;
extern int local_frame_depth;
extern int g_spill_counter;
extern instr_t *prog;
extern int nprog;
extern int prog_cap;
extern char entry_label[MAX_SYMLEN];
extern int g_uses_heap;
extern int g_uses_float;
extern global_pin_t global_pins[MAX_GLOBAL_PINS];
extern int nglobal_pins;
extern int g_pin_load_start_idx;
extern int g_pin_load_end_idx;
extern param_scope_t current_params;
extern int in_function;
extern func_sig_t *func_sigs;
extern int nfunc_sigs;
extern int func_sigs_cap;
extern extern_sig_t *externs;
extern int nexterns;
extern int externs_cap;
extern char g_extern_libs[64][MAX_SYMLEN];
extern int g_n_extern_libs;
extern int g_libc_linked;
extern int g_libc_init_seen;
extern int g_entry_is_main_shorthand;
extern int g_in_main_block;
extern int g_main_block_frame_depth;
extern char g_entry_symbol_override[MAX_SYMLEN];
extern int g_line_no;
extern const char *g_filename;
extern const char *g_source_line;
extern target_t g_target;
extern jmp_buf g_recovery_point;
extern int g_recovery_active;
extern char g_collected_errors[MAX_COLLECTED_ERRORS][MAX_LINE + 128];
extern int g_ncollected_errors;
extern int g_errors_truncated;
extern macro_def_t *macros;
extern int nmacros;
extern int macros_cap;
extern int g_macro_expand_counter;
extern equ_def_t *equs;
extern int nequs;
extern int equs_cap;
extern alias_def_t *aliases;
extern int naliases;
extern int aliases_cap;
extern enum_member_t *enum_members;
extern int nenum_members;
extern int enum_members_cap;
extern struct_def_t *struct_defs;
extern int nstruct_defs;
extern int struct_defs_cap;
extern local_frame_t local_frame_stack[MAX_LOCAL_FRAME_DEPTH];
extern scope_t scope_stack[MAX_SCOPE_DEPTH];
extern int scope_depth;
extern int g_label_counter;
extern pp_line_t *pp_lines;
extern int npp_lines;
extern int pp_lines_cap;
extern char **file_names;
extern int nfile_names;
extern int file_names_cap;
extern const char **included_paths;
extern int nincluded_paths;
extern int included_paths_cap;
extern const char *include_stack[MAX_INCLUDE_DEPTH];
extern int ninclude_stack;
extern long g_foot_addr;
extern int g_foot_seen;
extern int g_heap_size_seen;
extern int g_init_scratch_seen;
extern int g_finit_scratch_seen;
extern int g_argv_seen;
extern int g_argv_argc_reg;
extern int g_argv_argv_reg;
extern const char *SCRATCH_X86;
extern const char *SCRATCH_ARM;
extern const char *SCRATCH_RV;
extern const char *SCRATCH_RV2;
extern const char *SCRATCH_RV3;
extern int g_riscv_vadd_warned;

#endif /* CHARD_GLOBALS_H */
