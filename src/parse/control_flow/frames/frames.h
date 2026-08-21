/* frames.h -- auto-generated declarations for the 'locals_frames' module
 * (11 function(s), defined in frames.c). */
#ifndef CHARD_MOD_FRAMES_H
#define CHARD_MOD_FRAMES_H

#include "../../../chard_types.h"
#include "../../../chard_globals.h"

int in_local_frame(void);
local_frame_t *current_local_frame(void);
decl_t *declare__local(const char *name, int size_bytes);
decl_t *declare_local_array(const char *name, int size_bytes, int array_len);
decl_t *declare_local_struct(const char *name, struct_def_t *sd);
void open_local_frame(void);
void note_written_register(const operand_t *d, int *out, int *out_isfloat, int *out_isf32, int *n);
int scan_written_registers(int from, int to, int *out, int *out_isfloat, int *out_isf32);
int relocate_frame_close_before_rets(int frame_open_idx, int frame_close_idx, int frame_bytes,
                                             int *out_ends_in_ret, int **out_ret_idxs, int *out_nrets);
void wrap_function_body(int frame_open_idx, int frame_close_idx, int nparams, int ret_reg,
                                int ends_in_ret, const int *ret_idxs, int nrets);
void close_local_frame(void);

#endif /* CHARD_MOD_FRAMES_H */
