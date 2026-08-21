/* chard_globals.c -- auto-generated: single definition point for every
 * shared mutable global (see chard_globals.h). */
#include "chard_globals.h"

long g_heap_size_bytes = DEFAULT_HEAP_SIZE_BYTES;
int g_init_scratch_reg = DEFAULT_INIT_SCRATCH_REG;
int g_finit_scratch_reg = DEFAULT_FINIT_SCRATCH_REG;
out_mode_t g_mode = MODE_BARE;
int g_mode_seen = 0; /* guards against '| mode' appearing twice,
                                same as g_heap_size_seen for '%sheap' */
decl_t *decls = NULL;
int ndecls = 0;
int decls_cap = 0;
int local_frame_depth = 0;
int g_spill_counter = 0;
instr_t *prog = NULL;
int nprog = 0;
int prog_cap = 0;
char entry_label[MAX_SYMLEN] = "_start";
int g_uses_heap = 0; /* set when any alloc() is parsed */
int g_uses_float = 0; /* set when any float decl/op is parsed --
                                  RISC-V needs this to know whether to
                                  declare the D extension (see
                                  emit__riscv); x86-64/AArch64 don't need
                                  an extension declaration since SSE2/
                                  AArch64 FP are baseline, not optional,
                                  on those targets. */
global_pin_t global_pins[MAX_GLOBAL_PINS];
int nglobal_pins = 0;
int g_pin_load_start_idx = -1;
int g_pin_load_end_idx = -1;
param_scope_t current_params = {0};
int in_function = 0; /* 0 = no function's parameters are active */
func_sig_t *func_sigs = NULL;
int nfunc_sigs = 0;
int func_sigs_cap = 0;
extern_sig_t *externs = NULL;
int nexterns = 0;
int externs_cap = 0;
char g_extern_libs[64][MAX_SYMLEN];
int g_n_extern_libs = 0;
int g_libc_linked = 0;   /* set by 'libc-init;' -- see block comment above */
int g_libc_init_seen = 0; /* guards against calling libcall() before libc-init */
int g_entry_is_main_shorthand = 0; /* set when the entry block was written as
    '@main { ... }' rather than '@start { ... }' -- '@main' is sugar for '@start' plus
    an implicit leading 'libc-init;' (see the '@'-label parse site). Recorded separately
    from g_libc_linked/g_libc_init_seen (which '@main' also sets) purely so a
    redundant hand-written 'libc-init;' right after '@main {' can be diagnosed as
    "you already got this from @main" instead of the ordinary
    "'libc-init' may only appear once" -- see that parse block's comment. */
int g_in_main_block = 0;      /* 1 while the parser is lexically inside the
    '@main { ... }' block (including inside any nested @label {} used for scoping, if/
    while/for bodies, etc within it), 0 otherwise. Drives the 'extern' scope
    restriction below: extern declares an ABI contract for a libc/externally-linked
    symbol, and libc is only ever linked in at all via '@main' (or 'libc-init;'), so an
    extern written anywhere else in the file -- file scope, inside an ordinary @label,
    inside a non-libc @start -- has no libc-linked program to belong to. Restricting it
    to @main's lexical body keeps every extern visibly co-located with the libc-linked
    entry point it exists for, rather than scattered file-wide with no positional
    discipline. */
int g_main_block_frame_depth = -1; /* local_frame_depth value @main's block sits
    at once opened (i.e. the depth *after* open_local_frame() runs for it) -- g_in_main_block
    is cleared exactly when close_local_frame() pops back below this depth, so nested
    @label {}/if/while/for blocks inside @main don't prematurely end the "inside @main"
    region just because they open/close their own scope along the way. */
char g_entry_symbol_override[MAX_SYMLEN] = ""; /* set by '%entrysym "name"',
    empty if not used -- see do_entry_symbol_directive and apply_entry_symbol_override.
    Takes priority over both the freestanding '_start' default and the
    libc-init-triggered 'main' retarget, since a directive the user wrote
    on purpose should always win over either automatic choice. */
int g_line_no = 0;
const char *g_filename = "";
const char *g_source_line = NULL;
target_t g_target = TARGET_X86_64;
jmp_buf g_recovery_point;
int g_recovery_active = 0;
char g_collected_errors[MAX_COLLECTED_ERRORS][MAX_LINE + 128];
int g_ncollected_errors = 0;
int g_errors_truncated = 0; /* set once MAX_COLLECTED_ERRORS is hit,
    so the final summary can say so rather than silently stop at 50 */
macro_def_t *macros = NULL;
int nmacros = 0;
int macros_cap = 0;
int g_macro_expand_counter = 0; /* fresh id per call-site expansion, for @label hygiene */
equ_def_t *equs = NULL;
int nequs = 0;
int equs_cap = 0;
alias_def_t *aliases = NULL;
int naliases = 0;
int aliases_cap = 0;
enum_member_t *enum_members = NULL;
int nenum_members = 0;
int enum_members_cap = 0;
struct_def_t *struct_defs = NULL;
int nstruct_defs = 0;
int struct_defs_cap = 0;
local_frame_t local_frame_stack[MAX_LOCAL_FRAME_DEPTH];
scope_t scope_stack[MAX_SCOPE_DEPTH];
int scope_depth = 0;
int g_label_counter = 0;
pp_line_t *pp_lines = NULL;
int npp_lines = 0;
int pp_lines_cap = 0;
char **file_names = NULL;
int nfile_names = 0;
int file_names_cap = 0;
const char **included_paths = NULL;
int nincluded_paths = 0;
int included_paths_cap = 0;
const char *include_stack[MAX_INCLUDE_DEPTH];
int ninclude_stack = 0;
long g_foot_addr = 0;
int g_foot_seen = 0;
int g_heap_size_seen = 0;
int g_init_scratch_seen = 0;
int g_finit_scratch_seen = 0;
int g_argv_seen = 0;
int g_argv_argc_reg = 0;
int g_argv_argv_reg = 0;
const char *SCRATCH_X86 = target_defs[TARGET_X86_64].scratch; /* not in the r1-r12 mapping */
const char *SCRATCH_ARM = target_defs[TARGET_AARCH64].scratch; /* not in the x0-x11 mapping */
const char *SCRATCH_RV = target_defs[TARGET_RISCV].scratch;
const char *SCRATCH_RV2 = "t1";
const char *SCRATCH_RV3 = "t2"; /* only needed by OP_ISTORE, where SCRATCH_RV/RV2
                                           are both already committed to the address calc */
int g_riscv_vadd_warned = 0;
