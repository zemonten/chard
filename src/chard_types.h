/* chard_types.h -- auto-generated: shared typedefs, structs, enums,
 * and preprocessor macros used across every module. Order-preserving
 * relative to the original chard.c since later typedefs depend on
 * earlier ones. Lookup tables used by only one module (e.g. the
 * instruction-mnemonic tables) live in that module's .c file instead. */
#ifndef CHARD_TYPES_H
#define CHARD_TYPES_H

#include <stdio.h>
#include <stdint.h>
#include <setjmp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <setjmp.h>
#include <limits.h>
#include <stdarg.h>
#define DA_INIT_CAP 16
#define DA_ENSURE(arr, cap, count, type) \
    do { \
        if ((count) >= (cap)) { \
            int new_cap_ = (cap) == 0 ? DA_INIT_CAP : (cap) * 2; \
            type *new_arr_ = realloc((arr), (size_t)new_cap_ * sizeof(type)); \
            if (!new_arr_) { perror("realloc"); exit(1); } \
            (arr) = new_arr_; \
            (cap) = new_cap_; \
        } \
    } while (0)
#define DA_ENSURE_N(arr, cap, need, type) \
    do { \
        if ((need) > (cap)) { \
            int new_cap_ = (cap) == 0 ? DA_INIT_CAP : (cap); \
            while (new_cap_ < (need)) new_cap_ *= 2; \
            type *new_arr_ = realloc((arr), (size_t)new_cap_ * sizeof(type)); \
            if (!new_arr_) { perror("realloc"); exit(1); } \
            (arr) = new_arr_; \
            (cap) = new_cap_; \
        } \
    } while (0)
#define MAX_LINE        512
#define MAX_TOKENS      16
#define MAX_SYMLEN      64
#define MAX_MACRO_PARAMS   8
#define MAX_MACRO_EXPAND_DEPTH 16 /* guards against macro-calls-itself infinite recursion */
#define DEFAULT_HEAP_SIZE_BYTES (1 << 20) /* 1 MiB */
#define HEAP_SYM      "__heap"
#define HEAP_PTR_SYM  "__heap_ptr"
#define DEFAULT_INIT_SCRATCH_REG 12
#define DEFAULT_FINIT_SCRATCH_REG 7
typedef enum { TARGET_X86_64, TARGET_AARCH64, TARGET_RISCV } target_t;
typedef enum { MODE_ELF, MODE_BARE } out_mode_t;
typedef enum { SEC_DATA, SEC_BSS, SEC_LOCAL, SEC_RODATA } section_t;
#define MAX_STRLEN 256
typedef struct {
    char name[MAX_SYMLEN];
    section_t section;
    int size_bytes;      /* from iN/fN; 0 for volatile ascii (uses str_val/
                             str_len directly), 1 for bss ascii */
    int has_init;
    long init_value;
    int is_ascii;
    int is_float;        /* 'fK' decl; real value is in init_fvalue */
    double init_fvalue;  /* SEC_DATA + is_float: full-precision value;
                             narrowed to f32/f64 at emission */
    char str_val[MAX_STRLEN];  /* raw text, escapes already resolved */
    int str_len;               /* byte length, computed at parse time */
    int local_offset;    /* SEC_LOCAL: byte offset below fp ([fp -
                             local_offset]), valid during parsing only --
                             codegen uses its own copy in operand_t */
    int local_depth;     /* SEC_LOCAL: local_frame_depth at declaration;
                             a deeper reference walks (own depth -
                             local_depth) saved-fp links to reach it */
    int array_len;        /* SEC_LOCAL: element count for 'local iK
                              name[N];' (0 = scalar); local_offset is
                              element 0's offset. SEC_BSS: same, for
                              'bss iK name[N];' (no initializer, as with
                              any bss decl); SEC_BSS+is_ascii: byte
                              capacity of the buffer. SEC_DATA+
                              is_data_array: count of data_vals[]. */
    int is_data_array;   /* 'data iK name[] = v1,v2,...;' -- named
                             initialized SEC_DATA array (a lookup/jump
                             table in one line); reuses has_init/
                             size_bytes like a scalar volatile decl */
    long *data_vals;      /* is_data_array values, narrowed to size_bytes
                              at emission; grown via DA_ENSURE. Used when
                              !is_float (see data_fvals otherwise). Slots
                              where data_val_is_label[v] is set are NOT
                              meaningful here -- see data_val_labels. */
    int data_vals_cap;
    double *data_fvals;   /* is_data_array + is_float: same as data_vals
                              but real doubles, narrowed to IEEE-754 bits
                              at emission */
    int data_fvals_cap;
    int *data_val_is_label;   /* is_data_array + !is_float only: parallel
                              to data_vals, one flag per element. Set when
                              that element was written as '&label' rather
                              than a numeric literal, i.e. this is a
                              genuine jump-table entry -- the element
                              holds a code address, not a plain integer.
                              Grown in lockstep with data_vals via
                              DA_ENSURE (same cap, data_vals_cap). NULL
                              whenever no element in this array is a
                              label (the common, non-jtable case) so
                              existing numeric-only 'data'/'volatile'
                              arrays cost nothing extra. */
    char **data_val_labels;   /* is_data_array + !is_float only: parallel
                              to data_vals, valid only where
                              data_val_is_label[v] is set, holding that
                              element's label text (e.g. "loop_body") to
                              emit as-is -- resolved by the assembler at
                              build time exactly like OP_JMP's dst.sym is,
                              not validated against a label table here.
                              Same cap as data_vals (data_vals_cap). NULL
                              alongside data_val_is_label == NULL. */
    char struct_type_name[MAX_SYMLEN]; /* SEC_LOCAL struct instance
                              ('local StructName name;'): size_bytes==1,
                              array_len==struct's total_size, reserved as
                              an opaque byte region. 'name.field' resolved
                              via struct_defs[] in parse_instr_line. */
} decl_t;
typedef enum {
    MEM_ORDER_SEQ_CST = 0, /* '%seq_cst' or no suffix -- the default */
    MEM_ORDER_RELAXED,     /* '%relaxed' */
    MEM_ORDER_ACQUIRE,     /* '%acquire' */
    MEM_ORDER_RELEASE,     /* '%release' */
    MEM_ORDER_ACQ_REL,     /* '%acq_rel' */
} mem_order_t;
typedef enum {
    OP_MOV, OP_LOAD, OP_STORE, OP_LEA,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR,
    OP_NOT, OP_NEG, /* unary: dst = ~src / dst = -src; same 'src > dst'
                        shape as the binary two-operand ops (src is the
                        sole input, no implicit dst-as-second-operand),
                        so 'not r1 > r1' negates in place same as any
                        other register the same way 'mv r1 > r1' would
                        be a no-op copy. */
    OP_ROTL, OP_ROTR, /* rotl/rotr COUNT > dst; dst = rotate-left/right(dst,
                          COUNT), same 'dst = dst OP src' shape as
                          shl/shr (COUNT is the src operand, dst is both
                          the value rotated and the destination). Always
                          a full 64-bit register rotate -- Chard registers
                          are opaque 64-bit values the same way OP_NOT/
                          OP_NEG treat them, so there's no separate
                          32-bit rotate form. COUNT is taken mod 64 by
                          the underlying hardware op on all 3 targets
                          (x86-64 rol/ror, AArch64 ror -- rotate-left is
                          synthesized as a right-rotate by 64-COUNT since
                          AArch64 has no native rotate-left instruction,
                          RISC-V rol/ror from the Zbb bitmanip extension)
                          the same way shl/shr's count already is on
                          x86-64 (masked to the low 6 bits by 'shl reg,
                          cl' in hardware). */
    OP_POPCOUNT, /* popcount SRC > dst; dst = number of set bits in SRC
                     (0-64). Unary, same 'src > dst' shape as OP_NOT/
                     OP_NEG. x86-64 uses the popcnt instruction (present
                     on all x86-64-v2+ baselines this project already
                     assumes elsewhere), AArch64 uses cnt (per-byte
                     popcount in a SIMD register) + addv to sum the
                     bytes, RISC-V uses cpop (Zbb). */
    OP_CLZ, /* clz SRC > dst; dst = count of leading zero bits in SRC,
                treating SRC as a full 64-bit value (clz of 0 is 64,
                matching every target's native semantics -- there is no
                separate "undefined at zero" case to guard here, unlike
                C's __builtin_clz). Unary, 'src > dst' shape. x86-64
                uses lzcnt (not bsr, which is undefined at zero and
                counts from the wrong end), AArch64 uses clz natively,
                RISC-V uses clz (Zbb). */
    OP_CTZ, /* ctz SRC > dst; dst = count of trailing zero bits in SRC
                (ctz of 0 is 64, same "no undefined case" contract as
                OP_CLZ). Unary, 'src > dst' shape. x86-64 uses tzcnt,
                AArch64 has no native ctz so it's synthesized as
                rbit+clz (bit-reverse then count leading zeros of the
                reversed value), RISC-V uses ctz (Zbb). */
    OP_SAT_ADD, OP_SAT_SUB, /* sat_add/sat_sub SRC > dst; dst = dst +/-
                                src, clamped to [INT64_MIN, INT64_MAX]
                                instead of wrapping. Signed only. Each
                                backend does the ordinary add/sub, checks
                                overflow via hardware flags (x86-64 OF,
                                AArch64 V; RISC-V compares operand/result
                                signs manually), and clamps if needed. */
    OP_SEXT, /* sextN SRC > rDST; dst = SRC's low N bits, sign-extended
                 to fill the full 64-bit register. N in {8,16,32} (no
                 sext64 -- a 64-bit value already fills the register,
                 there's nothing to extend). Unary, 'src > dst' shape,
                 elem_size holds N/8 -- same suffix grammar as iloadN's
                 's' variant (iload8s/16s/32s), but for a value that's
                 already in a register/immediate rather than being
                 loaded from memory. x86-64 uses movsx (movsxd for the
                 32-bit case, since plain movsx tops out at a 16-bit
                 source), AArch64 uses sxtb/sxth/sxtw natively, RISC-V
                 has no sub-word sign-extend below Zbb's sext.b/sext.h
                 (and nothing at all for 32-bit short of the always-
                 available addiw idiom), so it's synthesized as
                 shift-left-then-arithmetic-shift-right by (64-N). */
    OP_ZEXT, /* zextN SRC > rDST; dst = SRC's low N bits, zero-extended
                 to fill the full 64-bit register. N in {8,16,32}, same
                 "no zext64" reasoning as OP_SEXT. Unary, 'src > dst'
                 shape. All three backends implement this as a plain
                 mask (x86-64/RISC-V: and with the right bitmask;
                 AArch64: uxtb/uxth natively, uxtw via a 32-bit mov
                 which the architecture itself zero-extends to 64 bits) --
                 there's no per-target instruction-selection story here
                 the way there is for OP_SEXT, since zero-extension is
                 just "clear the high bits" on every target. */
    OP_CMP, OP_JMP, OP_JE, OP_JNE, OP_JG, OP_JL,
    OP_JGE, OP_JLE,             /* signed >=, <= */
    OP_JA, OP_JB, OP_JAE, OP_JBE, /* unsigned >, <, >=, <= */
    OP_ASSERT,    /* assert LHS OP RHS; -- runtime postcondition. Reuses
                       if/while's OP_CMP (parse_cond_and_emit_cmp),
                       followed by an OP_ASSERT carrying the *inverted*
                       condition's jump opcode (assert_jmp_op below): if
                       that inverted condition is true, jump around an
                       inline trap; else fall through. No block/scope. */
    OP_CALL, OP_RET,
    OP_EXIT, OP_STDOUT, OP_SYSCALL, OP_READ, OP_WRITE,
    OP_HALT,       /* halt; -- BARE mode's counterpart to OP_EXIT. Not a
                       syscall (no kernel in BARE mode) -- a raw "stop
                       the CPU" instruction (x86-64 hlt, AArch64/RISC-V
                       wfi) wrapped in an infinite loop so a wakeup
                       interrupt doesn't fall through. Never returns. */
    OP_LIBC_INIT,  /* libc-init; -- switches entry from freestanding
                       _start/raw-syscall to libc-linked 'main', emits
                       extern decls for 'extern name(n);'. See "libc
                       interop" near parse__file. */
    OP_LIBC_CALL,  /* libcall name(arg1,...,arg6) [> rX]; -- calls an
                       extern libc function via the real C ABI (unlike
                       call(), which uses Chard's r1-r12 convention).
                       Args in ins->args[]; dst optionally gets the
                       return value (rax/x0/a0). */
    OP_ALLOC,     /* bump-allocate N bytes from the static heap arena */
    OP_HEAP_RESET, /* hp-reset; -- rewinds __heap_ptr to &__heap; the
                       only reclamation Chard v1 offers (no free()) */
    OP_ILOAD,     /* indexed heap load:  iloadN rBASE[rIDX] > rDST  */
    OP_ISTORE,    /* indexed heap store: istoreN rSRC > rBASE[rIDX] */
    OP_LALOAD,    /* indexed local-array load:  laloadN name[rIDX] > rDST */
    OP_LASTORE,   /* indexed local-array store: lastoreN rSRC > name[rIDX] */
    OP_BCMP,      /* bcmpN rDST, rPTR1, rPTR2, LEN; bcmp/memcmp-style:
                       rDST = 0 if the LEN bytes at rPTR1/rPTR2 match,
                       else 1. equal=0 so 'if rDST == 0 {}' reads like
                       any other comparison. A value result, not a
                       flags op -- Chard has no flags-into-register path.
                       LEN is a byte count regardless of N (N is just the
                       per-chunk read width codegen loops with). rDST,
                       rPTR1, rPTR2 must be three distinct registers. */
    OP_BCOPY,     /* bcopyN rDST, rSRC, LEN; bcopy/memcpy-style: copies
                       LEN bytes rSRC->rDST (dst,src,len arg order, like
                       memcpy). LEN is a byte count regardless of N; no
                       bounds checking; memcpy (not memmove) semantics --
                       overlap is undefined. rDST/rSRC must differ. */
    OP_HFIELD_LOAD,  /* heap struct field load: hfieldN rBASE.field > rDST.
                          field's byte offset is a compile-time constant
                          (const_offset), added with no scaling -- kept
                          separate from OP_ILOAD since the addressing
                          mode genuinely differs (fixed add vs scaled
                          index). */
    OP_HFIELD_STORE, /* mirrors OP_HFIELD_LOAD: hfieldN rSRC > rBASE.field */
    OP_XLOAD,     /* scaled-index-plus-displacement heap load:
                       xloadN rBASE[rIDX*SCALE+DISP] > rDST, i.e.
                       rDST = *(rBASE + rIDX*SCALE + DISP). Generalizes
                       OP_ILOAD (fixed scale, no disp) and OP_HFIELD_LOAD
                       (disp only, no index). SCALE must be 1/2/4/8,
                       matching real addressing hardware (x86 SIB,
                       AArch64 LSL #0-3); DISP may be negative. Reuses
                       OP_ILOAD's base_reg/idx_reg/elem_size and
                       OP_HFIELD_LOAD's const_offset; SCALE gets its own
                       xaddr_scale field since width and scale are
                       independent here. */
    OP_XSTORE,    /* mirrors OP_XLOAD: xstoreN rSRC > rBASE[rIDX*SCALE+DISP] */
    OP_PTRADD,    /* address-only pointer arithmetic, no memory access:
                       ptradd rBASE[rIDX*SCALE+DISP] > rDST  (or the
                       index-free 'rBASE + DISP > rDST' form). Same
                       fields/syntax as OP_XLOAD but computes the address
                       into rDST instead of dereferencing it (OP_LEA's
                       relationship to a plain load). Useful for building
                       a pointer to hand off elsewhere without faking a
                       throwaway load/store. Omitted idx_reg forces
                       scale=1 for the plain 'rBASE + DISP' form. */
    OP_PTRSUB,    /* mirrors OP_PTRADD with DISP negated at parse time:
                       rDST = rBASE - (rIDX*SCALE + DISP). Its own opcode
                       so 'walk backward by N elements' reads naturally,
                       like 'sub' does for register arithmetic. */
    OP_PUSH,      /* push src (reg or imm) onto the stack, sp -= 8 */
    OP_POP,       /* pop the stack into dst (reg, optional), sp += 8 */
    /* Atomics. All operate on a memory location (a volatile/bss global or
       a local -- same 'SYM' generality as load/store, see §6.1) holding an
       integer of width N (8/16/32/64, matching the laloadN/istoreN
       convention elsewhere). Every read-modify-write form here returns
       the location's OLD value (fetch-and-op semantics, matching what
       lock xadd / ldaddal / amoadd.d all give you directly or can cheaply
       emulate) -- Chard doesn't offer a "don't-care-about-old-value"
       variant, since the caller can just ignore the returned register if
       they don't need it, and offering two forms per op would double the
       opcode surface for zero expressiveness gain.
       Ordering: every atomic op here defaults to sequentially consistent
       (the strongest ordering) on all three targets when no suffix is
       written, matching the mnemonics' original (pre-ordering-parameter)
       behavior -- see mem_order_t's comment for why the default has to
       be seq_cst rather than something cheaper. An optional trailing
       '%relaxed' / '%acquire' / '%release' / '%acq_rel' suffix (see
       parse_mem_order_suffix) opts into a weaker, cheaper ordering on
       AArch64/RISC-V where that's a real performance lever; x86-64
       ignores the suffix entirely (lock-prefixed RMWs are already
       full-barrier-strength in hardware, there's no cheaper form to
       select). */
    OP_ATOMIC_ADD,   /* atom+ SRC > SYM > rDST [%ORDERING]; SYM += SRC, rDST = old value */
    OP_ATOMIC_SUB,   /* atom- SRC > SYM > rDST [%ORDERING]; SYM -= SRC, rDST = old value */
    OP_ATOMIC_AND,   /* atom& SRC > SYM > rDST [%ORDERING]; SYM &= SRC, rDST = old value */
    OP_ATOMIC_OR,    /* atom| SRC > SYM > rDST [%ORDERING]; SYM |= SRC, rDST = old value */
    OP_ATOMIC_XOR,   /* atom^ SRC > SYM > rDST [%ORDERING]; SYM ^= SRC, rDST = old value */
    OP_ATOMIC_SWAP,  /* atom<> SRC > SYM > rDST [%ORDERING]; SYM = SRC, rDST = old value */
    OP_ATOMIC_MAX,   /* atom>< SRC > SYM > rDST [%ORDERING]; SYM = max(SYM, SRC)
                         (signed), rDST = old value. Same fetch-and-op
                         shape as atom+/atom-/etc. Signed only, matching
                         how add/sub/mul are sign-agnostic at the IR
                         level with signedness only entering at
                         comparison/jump/div. */
    OP_ATOMIC_MIN,   /* atom<>< SRC > SYM > rDST [%ORDERING]; SYM = min(SYM, SRC)
                         (signed), rDST = old value. See OP_ATOMIC_MAX. */
    OP_ATOMIC_CAS,   /* atom= SYM, rEXPECTED, rDESIRED > rDST [%ORDERING];
                         if SYM == rEXPECTED { SYM = rDESIRED; rDST = 1 }
                         else { rDST = 0 } -- rDST reports success, not
                         the old value (C11/Rust compare_exchange style). */
    OP_FENCE,        /* fence [%ORDERING]; -- memory barrier. Default is
                         full seq_cst (mfence/dmb ish/fence rw,rw); an
                         optional '%ORDERING' suffix requests a weaker
                         one -- see mem_order_t and parse_mem_order_suffix. */
    /* Floats: a separate f1-f8 register file from r1-r12 (see
       operand_t.is_float), so int/float never contend for a slot.
       Always f64 internally -- Chard has no per-register width tag, so
       f32 exists only as a storage format (OP_FLOAD/OP_FSTORE widen on
       load, narrow on store). Keeps the opcode surface flat (one fadd,
       not fadd32/fadd64) at the cost of always computing in double
       precision. */
    OP_FMOV,      /* fmov SRC > fDST; SRC is a float register or a
                       float-literal immediate (e.g. 3.14) */
    OP_FLOAD,     /* fload SYM > fDST;  load f32/f64 SYM (bss/volatile/
                       local) into fDST, widening f32 to f64 in the
                       register -- see OP_FLOAD codegen for the exact
                       per-target instruction */
    OP_FSTORE,    /* fstore fSRC > SYM;  store fSRC (always held at f64
                       precision -- see the block comment above) into
                       SYM, narrowing to f32 first if SYM was declared
                       f32 */
    OP_FADD, OP_FSUB, OP_FMUL, OP_FDIV,  /* fDST = fDST OP fSRC (same
                       destructive two-operand shape as OP_ADD/etc) */
    OP_FCMP,      /* fcmp fRHS > fLHS; same src>dst role convention as
                       OP_CMP, sets the integer condition flags from a
                       float comparison so OP_JE/JG/JL/... work unchanged
                       (mirrors x86 ucomisd setting ZF/PF/CF). Unordered
                       (NaN) compares as "greater" on all three targets;
                       no separate NaN-aware jump form in v1. */
    OP_I2F,       /* i2f rSRC > fDST;  convert integer rSRC to float */
    OP_F2I,       /* f2i fSRC > rDST;  convert float fSRC to integer
                       (truncating toward zero, matching C's (long)x) */
    OP_FSQRT,     /* fsqrt fSRC > fDST; fDST = sqrt(fSRC). Unary (src/dst
                       may be the same register, like fmov). Negative
                       input produces NaN (native IEEE-754 behavior), no
                       domain check added. */
    OP_FABS,      /* fabs fSRC > fDST; fDST = |fSRC|. Sign-bit clear,
                       unconditional on every target. */
    OP_FNEG,      /* fneg fSRC > fDST; fDST = -fSRC. Sign-bit flip (XOR
                       mask), not 0-src -- flip correctly negates signed
                       zero/NaN, which subtraction wouldn't. AArch64/
                       RISC-V have native fneg/fneg.d, no mask needed. */
    OP_FMIN, OP_FMAX,  /* fmin/fmax SRC > fDST; destructive two-operand
                       shape like fadd/fsub/fmul/fdiv. NaN behavior
                       follows each target's native instruction ("NaN
                       loses to a real number" -- x86 minsd/maxsd,
                       AArch64 fmin/fmax, RISC-V fmin.d/fmax.d agree). */
    OP_FMA,       /* fma fA, fB, fC > fDST; fDST = (fA*fB)+fC as one
                       fused multiply-add, single rounding step (matches
                       x86 vfmadd213sd, AArch64 fmadd, RISC-V fmadd.d).
                       The one float op with three sources -- reuses the
                       atomics' result_reg/cas_expected/cas_desired trio,
                       relabeled fa/fb/fc. All four operands must be
                       registers; no immediate operand (no native fma
                       form takes one) -- fmov literals in first. */
    OP_VADD,      /* vadd fSRC > fDST;  packed 2x-f64 SIMD add: treats
                       fDST/fSRC as a 128-bit register holding two
                       adjacent f64 lanes, added lane-wise in one
                       instruction (x86 SSE2 addpd, AArch64 NEON fadd
                       v.2d). Same destructive src>dst shape as fadd,
                       same f1-f8 register file. No float-literal source.

                       vsub/vmul/vdiv/vmin/vmax (below) share this shape
                       with a different lane-wise op; only op-specific
                       notes are repeated for them.

                       RISC-V has no fixed-width packed-float op (its
                       SIMD story is the Vector extension, a separate
                       v0-v31 file needing runtime vsetvli config, not a
                       drop-in mnemonic). Codegen falls back to two
                       sequential scalar f*.d instructions, correct but
                       without the parallelism the mnemonic implies, and
                       prints a one-time stderr note. See emit__riscv's
                       OP_VADD case. Lane 1 (unused/discarded) is 0.0 in
                       the fallback, matching the real-SIMD targets'
                       zero-extended upper lane. */
    OP_VSUB,      /* vsub fSRC > fDST; packed 2x-f64 subtract -- see
                       OP_VADD; subpd/fsub v.2d, fsub.d in the RISC-V
                       unroll. */
    OP_VMUL,      /* vmul fSRC > fDST; packed 2x-f64 multiply -- see
                       OP_VADD; mulpd/fmul v.2d, fmul.d in the RISC-V
                       unroll. */
    OP_VDIV,      /* vdiv fSRC > fDST; packed 2x-f64 divide -- see
                       OP_VADD; divpd/fdiv v.2d, fdiv.d in the RISC-V
                       unroll. Lane 1 is 0.0/0.0 = NaN on all three
                       targets here (unlike add/sub/mul, whose identity
                       happens to still be 0.0) -- harmless, since it's
                       discarded either way. */
    OP_VMIN,      /* vmin fSRC > fDST; packed 2x-f64 minimum -- see
                       OP_VADD; minpd/fmin v.2d, fmin.d in the RISC-V
                       unroll. Same "NaN loses" convention as scalar
                       fmin. */
    OP_VMAX,      /* vmax fSRC > fDST; packed 2x-f64 maximum -- see
                       OP_VMIN; maxpd/fmax v.2d, fmax.d in the RISC-V
                       unroll. */
    OP_VSQRT,     /* vsqrt fSRC > fDST;  packed 2x-f64 SIMD square root
                       -- the packed-lane counterpart of scalar fsqrt,
                       same relationship vadd/etc have to fadd/etc, but
                       unary like fsqrt itself rather than destructive
                       two-operand like vadd/vmin/etc: src is the whole
                       input, dst receives the result, no dst-as-input
                       combine step (x86-64 SSE2 sqrtpd, AArch64 NEON
                       fsqrt v.2d). Negative lanes produce NaN in that
                       lane only, same IEEE-754 sqrt convention as
                       scalar fsqrt -- no cross-lane interaction, each
                       lane computes independently. No float-literal
                       source, same register-only rule as the rest of
                       the vN family (see OP_VADD) -- unary or not, a
                       packed op still has no single-float literal
                       spelling.

                       RISC-V's fallback unrolls into two scalar
                       fsqrt.d's, same technique as OP_VADD's fallback
                       but simpler since there's no second operand to
                       synthesize an identity for -- lane 0 is
                       fsqrt.d'd for real, lane 1 is fsqrt.d applied to
                       a synthesized 0.0 (sqrt(0.0) = 0.0, matching the
                       zero-extended upper lane every OTHER unused lane
                       already holds on the two real-SIMD targets, same
                       zero-extension story as OP_VADD's opcode_t
                       comment). Shares OP_VADD's one-time stderr note
                       (see emit__riscv's case) since it's the same
                       underlying "no fixed-width packed register on
                       this target" gap. */
    OP_VABS,      /* vabs fSRC > fDST;  packed 2x-f64 SIMD absolute
                       value -- the packed-lane counterpart of scalar
                       fabs, unary like vsqrt above (see it for the
                       shared unary-vs-destructive shape note). Same
                       per-lane sign-bit-clear every target's scalar
                       fabs already does, just applied to both lanes at
                       once: x86-64 SSE2 andpd against the same
                       per-lane sign-clear mask OP_FABS's andpd already
                       uses (the mask is naturally 128 bits wide simply
                       by using andpd instead of andps -- no separate
                       "packed mask" needed, the same 64-bit mask
                       pattern repeats in both lanes because
                       0x7fffffffffffffff written into fscratch via
                       movq zero-extends the upper lane the same way
                       every other f-register write does, and 0 & 0 in
                       the upper lane leaves it exactly 0.0, still
                       correct even though only the low lane's mask
                       bits are semantically "real"), AArch64 NEON fabs
                       v.2d (a dedicated packed instruction, no mask
                       needed, mirroring scalar fabs's own directness).

                       RISC-V's fallback unrolls into two scalar
                       fabs.d's; lane 1 is fabs.d applied to a
                       synthesized 0.0 (|0.0| = 0.0), same
                       zero-extension match as vsqrt's fallback.
                       Shares OP_VADD's one-time stderr note. */
    OP_VNEG,      /* vneg fSRC > fDST;  packed 2x-f64 SIMD negate -- the
                       packed-lane counterpart of scalar fneg (see
                       OP_FNEG's opcode_t comment for why fneg exists
                       as a sign-bit-flip rather than a 0-src subtract),
                       unary like vsqrt/vabs above. x86-64 SSE2 xorpd
                       against the same per-lane sign-flip mask
                       OP_FNEG's xorpd already uses (0x8000000000000000
                       repeated in both lanes the same zero-extension
                       way OP_VABS's andpd mask is -- the upper lane's
                       XOR against a zero-extended 0.0 mask-bit leaves
                       it exactly -0.0, still matching the "lane 1 is
                       whatever the identity op computes on the
                       zero-extended upper lane" story used throughout
                       the vN family, see OP_VADD), AArch64 NEON fneg
                       v.2d (dedicated packed instruction, no mask
                       needed).

                       RISC-V's fallback unrolls into two scalar
                       fneg.d's; lane 1 is fneg.d applied to a
                       synthesized 0.0, producing -0.0 -- distinct from
                       vsqrt/vabs's lane 1 (still 0.0), but this is not
                       a divergence from the real-SIMD targets any more
                       than OP_VDIV's NaN lane was: x86-64's xorpd and
                       AArch64's fneg v.2d both also flip the sign of a
                       zero-extended-to-0.0 upper lane into -0.0, so all
                       three targets agree the discarded lane is -0.0
                       here, same "genuinely matches hardware, just
                       isn't the 0.0 every OTHER op's lane 1 happens to
                       be" situation OP_VDIV already established.
                       Shares OP_VADD's one-time stderr note. */
    OP_VDUP,      /* vdup fSRC > fDST;  broadcast the scalar f64 in
                       fSRC's low 64 bits into both lanes of fDST --
                       the only way (besides vload from a real 2-element
                       array) to populate a packed vN register: without
                       it, turning one already-computed scalar into a
                       vN operand for vadd/vmul/etc meant round-tripping
                       through memory (fstore the scalar out, then vload
                       it back doubled), even though the value was
                       already sitting in a register. Unary like
                       vsqrt/vabs/vneg (src is the whole input, no
                       dst-as-input combine), register-only like the
                       rest of the vN family -- no float-literal source
                       (see OP_VADD's opcode_t comment; a literal would
                       just be a compile-time-constant broadcast the
                       caller could write into both lanes themselves via
                       two fmov's, not worth a codegen path). src and
                       dst may be the same register (in-place broadcast
                       -- reads the low lane before the high lane is
                       overwritten, so this is always safe).

                       x86-64: SSE3 movddup dst, src (dedicated
                       "move one double, duplicated" instruction -- not
                       part of baseline SSE2 like the rest of the vN
                       family's x86 forms, but SSE3 has shipped on every
                       x86-64 CPU since 2004, so no feature-detection
                       story is needed here any more than baseline SSE2
                       needs one for the others). AArch64: NEON
                       dup dst.2d, src.d[0] (dedicated broadcast-from-
                       lane-0 instruction, same directness as vneg's
                       fneg v.2d). RISC-V: no packed register (same "no
                       fixed-width packed register" gap as every other
                       vN op -- see OP_VADD), so this unrolls into two
                       ordinary scalar fmv.d.d's (register-to-register
                       double copy) writing the same source into both
                       of the destination's underlying f-registers --
                       simpler than vsqrt/vabs/vneg's fallback since
                       there's no per-lane computation at all, just two
                       copies of the same value. Shares OP_VADD's
                       one-time stderr note. */
    OP_VLOAD,     /* vload SYM > fDST;  load 128 bits (two adjacent f64
                       lanes) from SYM into fDST as one packed register
                       -- the memory-side counterpart of the vN
                       arithmetic family: without this (and OP_VSTORE
                       below), there was no way to get two real f64
                       values into one vN register in a single
                       instruction at all, only via two separate scalar
                       fmov/fload calls into two different f-registers,
                       which are NOT the same physical 128-bit register
                       vadd/vmin/etc operate on (see the OP_VADD
                       opcode_t comment -- Chard has no vector register
                       class of its own; a vN register IS an ordinary
                       f-register, just interpreted as two lanes wide by
                       vadd/etc's mnemonic choice, not by any special
                       population step). SYM must be a two-element f64
                       array (declared 'local/bss/volatile f64
                       name[2];' -- see the existing array-declaration
                       grammar; ordinary scalar f64/f32 symbols are
                       rejected, since they're only 8 bytes, not the 16
                       this op reads/writes as a unit).

                       x86-64: movupd, the UNALIGNED 128-bit SSE2
                       move -- not movapd. A two-element local array's
                       base offset is only guaranteed 8-byte aligned
                       (declare_local_array sets align = size_bytes,
                       i.e. 8 for an f64 element, not 16), and movapd
                       #GPs on a misaligned address, so movupd is the
                       only safe choice regardless of the small
                       constant-factor speed cost. AArch64: ldr/str
                       with a q-register (128-bit NEON load/store),
                       which -- unlike x86's aligned move forms --
                       doesn't require 16-byte alignment by default, so
                       no unaligned-vs-aligned split is needed there.
                       RISC-V: no packed load at all (same "no
                       fixed-width packed register" gap as every other
                       vN op -- see OP_VADD), so this unrolls into two
                       ordinary scalar fld.d's at offset +0 and +8,
                       sharing the same one-time stderr note the
                       arithmetic/unary vN ops already print. */
    OP_VSTORE,    /* vstore fSRC > SYM;  store fSRC's 128 bits (two
                       packed f64 lanes) into SYM as one unit -- the
                       write-side mirror of OP_VLOAD; see it for the
                       full rationale, the SYM shape requirement, and
                       the per-target alignment/instruction notes
                       (movupd / str q / two fsd.d's, in the same order
                       as OP_VLOAD's movupd / ldr q / two fld.d's). */
    OP_I2S,       /* i2s rSRC > rBUF, rLEN;  convert the integer in rSRC
                       to ASCII decimal digits (a leading '-' is written
                       for negative values), writing them starting at
                       the byte address held in rBUF -- any writable
                       pointer (alloc()'d heap, &local array, etc), same
                       as OP_ISTORE's rBASE. No null terminator is
                       written: Chard's strings are length-based (see
                       decl_t.str_len / OP_STDOUT), not C-style
                       null-terminated, so a trailing NUL would be an
                       unused extra byte here rather than a convention
                       anything else in Chard relies on. rLEN receives
                       the number of bytes written, so the caller always
                       knows exactly how much of rBUF was touched.
                       rSRC/rBUF/rLEN must be three distinct registers
                       (see parse-time check). */
    OP_S2I,       /* s2i rBUF, rLEN > rDST;  parse rLEN bytes of ASCII
                       decimal digits (an optional leading '-' is
                       honored) starting at the byte address in rBUF,
                       writing the resulting integer to rDST. Mirrors
                       i2s's length-based (not null-terminated) string
                       model: the caller supplies the length explicitly
                       rather than s2i scanning for a terminator, since
                       nothing else in Chard writes one. Undefined digits
                       (anything outside '0'-'9', except one optional
                       leading '-') are not validated -- same
                       no-bounds-checking philosophy as OP_ILOAD/
                       OP_ISTORE already document for heap memory. */
    OP_RAW,       /* raw "..."; -- emits the string verbatim into the
                      output stream at this point, for the current
                      target only. Opaque to Chard: no validation of the
                      text, no interaction with register/stack
                      bookkeeping. Escape hatch for instructions/
                      directives the IR doesn't model (see raw_text
                      on instr_t). */
    OP_RAWDATA,   /* bytes iK v1, v2, ...; or bytes fK v1.v, v2.v, ...; --
                      emits an anonymous, raw sequence of iK- or
                      fK-sized values directly into the instruction
                      stream at this point (a db/dw/dd/dq-equivalent
                      dropped inline, not declared up front like
                      'data'/'volatile'). Meant for jump tables, lookup
                      tables, or any blob of values that reads better
                      sitting next to the code that uses it. Has no name
                      of its own -- if code needs to address it, precede
                      it with a bare '@label:' (no '{'), which already
                      places an ordinary jump-target label right before
                      whatever instruction follows; see raw_data_* on
                      instr_t for the payload. */
    OP_LABEL,     /* pseudo-op: marks a label position (incl. entry) */
    OP_FRAME_OPEN,  /* pseudo-op: reserve frame_bytes on the stack for a
                        block's locals (sp -= frame_bytes, alignment-
                        rounded per target); frame_bytes is back-patched
                        once the block's '}' is reached */
    OP_FRAME_CLOSE  /* pseudo-op: give back the same frame_bytes (sp +=
                        frame_bytes); pairs with the OP_FRAME_OPEN that
                        opened this block */
} opcode_t;
typedef enum { OPND_NONE, OPND_REG, OPND_SYM, OPND_IMM, OPND_LABEL, OPND_LOCAL, OPND_ADDR } opnd_kind_t;
typedef struct {
    opnd_kind_t kind;
    int reg_num;               /* for OPND_REG: 1..12 (integer) or 1..8
                                   (float, when is_float is set), or 0 for sp */
    int is_sp;
    int is_float;               /* for OPND_REG: selects the f1-f8/s1-s8
                                    float register files instead of
                                    r1-r12. Kept as a separate flag rather
                                    than folding into opnd_kind_t (e.g. an
                                    OPND_FREG) since every existing
                                    'kind == OPND_REG' check throughout
                                    the file (is_mem_operand callers,
                                    parse-time validation, etc.) stays
                                    correct unchanged for a float
                                    register -- only the handful of sites
                                    that render a register name or pick a
                                    register file need to additionally
                                    branch on is_float, rather than every
                                    site that merely checks "is this a
                                    register at all" needing to learn a
                                    new enum value. Meaningless (left 0)
                                    on every other opnd_kind_t. */
    int is_f32;                 /* Only meaningful when is_float is set:
                                    0 selects f1-f8 (f64, double
                                    precision -- the original, and still
                                    default, float file), 1 selects s1-s8
                                    (f32, single precision, its own
                                    separate register file rather than a
                                    width tag on f1-f8 -- see the block
                                    comment above the float-op table for
                                    why: same reasoning that already kept
                                    integer and float registers from
                                    sharing one file applies again here,
                                    and it avoids a register silently
                                    holding "f64 bits" vs "f32 bits"
                                    depending on which instruction last
                                    touched it, which is exactly the kind
                                    of implicit state this split exists
                                    to remove). Deliberately a second
                                    orthogonal flag rather than promoting
                                    is_float itself to a 3-way enum: every
                                    one of the ~70 existing 'if (is_float)'
                                    / 'if (!is_float)' checks throughout
                                    the file already means exactly "is
                                    this a float register at all" and
                                    stays correct unchanged this way; only
                                    the small set of sites that actually
                                    pick a concrete register file or
                                    render a register name (see
                                    reg__name, the save/restore
                                    collection logic, and each float
                                    opcode's per-backend codegen) need to
                                    additionally branch on is_f32. Always
                                    0 when is_float is 0 (meaningless for
                                    an integer register) and left 0 by
                                    every pre-existing call site that
                                    zero-initializes operand_t via
                                    memset, so no existing parse site had
                                    to change to keep meaning "f64,
                                    exactly as before". */
    char sym[MAX_SYMLEN];      /* for OPND_SYM / OPND_LABEL; also kept set
                                   for OPND_LOCAL (the local's name) purely
                                   for diagnostics -- codegen addresses a
                                   local via local_offset/local_size below,
                                   never by re-resolving this name, since a
                                   local's decls[] entry (if any) may no
                                   longer exist by the time codegen runs */
    int is_addr_of;             /* for OPND_SYM or OPND_LOCAL: set when the
                                    operand was spelled '&SYM' or '&local'
                                    rather than plain 'SYM'/'local'. Mirrors
                                    global_pin_t.is_addr, which predates
                                    this and already proved the pattern for
                                    'global rN = &SYM;'. Meaning: codegen
                                    should materialize the operand's
                                    address into a register (the same
                                    computation 'lea SYM > rX' / 'lea local
                                    > rX' already does) instead of the
                                    default is_mem_operand behavior of
                                    loading the value stored *there*.
                                    Originally only consulted by
                                    OP_SYSCALL's argument marshalling (see
                                    each backend's OP_SYSCALL case) -- a
                                    syscall like bind/connect/sendto/
                                    recvfrom needs a pointer argument, and
                                    without this flag the old codegen would
                                    load the pointed-to bytes as if *they*
                                    were the pointer, a real bug this flag
                                    exists to fix. Extended to OPND_LOCAL so
                                    '&local' works the same way a local's
                                    address is already computed internally
                                    for OP_LEA/array-element addressing --
                                    see x86_addr_text/aarch64_local_base/
                                    riscv_local_base, all of which already
                                    branch on OPND_LOCAL regardless of this
                                    flag, so no address-computation code is
                                    new here, only the parse-time
                                    permission to reach it via '&'. '&SYM'/
                                    '&local' used anywhere OP_SYSCALL
                                    doesn't explicitly handle it is a
                                    parse-time error (see parse__operand)
                                    rather than silently falling back to
                                    value semantics, matching Chard's
                                    existing "no silent fallback" stance
                                    elsewhere (see e.g. sizeof's deliberate
                                    exception to that rule, which this is
                                    not). */
    long imm;                  /* for OPND_IMM; also doubles as the raw
                                    numeric address for OPND_ADDR (see
                                    parse__operand's '[EXPR]' handling)
                                    -- both are "a compile-time-constant
                                    long", so a second field would only
                                    duplicate this one under a different
                                    name. */
    double fimm;                /* for OPND_IMM when is_float is set: a
                                    float-literal immediate (e.g. the 3.14
                                    in 'fmov 3.14 > f1;'). imm/fimm are
                                    deliberately not unioned: is_float
                                    alone already disambiguates which one
                                    a reader should trust, and keeping
                                    them as separate fields means a
                                    stray write to one can never
                                    bit-clobber the other during parsing. */
    int local_offset;          /* for OPND_LOCAL: byte offset below fp (real address is [fp - local_offset]) */
    int local_size;            /* for OPND_LOCAL: 1/2/4/8. Also doubles as
                                    the access width for OPND_ADDR (an
                                    absolute address has no decls[] entry
                                    to size itself from the way OPND_SYM
                                    does, so '[EXPR]' must carry its own
                                    width -- see the loadK/storeK size
                                    suffix parsing in parse_instr_line and
                                    operand_mem_size's OPND_ADDR case).
                                    OPND_LOCAL and OPND_ADDR never occupy
                                    the same operand_t, so the reuse can't
                                    collide. */
    int frames_up;              /* for OPND_LOCAL: how many enclosing @label
                                    frames separate this reference from the
                                    block that actually declared the local
                                    (0 if declared in the block containing
                                    this reference). Codegen walks this many
                                    saved-fp links at runtime before
                                    applying local_offset -- see
                                    emit_*_local_addr in each backend. */
} operand_t;
typedef struct {
    opcode_t op;
    operand_t src;   /* left-hand operand in source syntax: op src > dst */
    operand_t dst;   /* right-hand operand: the destination */
    int is_entry;    /* only for OP_LABEL: true if this is the entry block */
    int is_func_start; /* only for OP_LABEL: true if this label opens a
                           '@name: { ... }' or '@name(...) -> rN: { ... }'
                           function-root block (see is_function_root) --
                           distinguishes a real function entry from a
                           plain jump-target label, so CFI emission
                           (.cfi_startproc on GAS targets, an FDE on
                           x86-64/NASM) knows exactly which labels need a
                           procedure record and which don't. Set right
                           after the label is pushed, once opens_block is
                           known (see the '@name:' parsing site) */
    int func_frame_close_idx; /* only for OP_LABEL when is_func_start: the
                           prog[] index of this function's OP_FRAME_CLOSE,
                           back-patched at '}' the same way frame_bytes is
                           (see close_local_frame) -- codegen needs this to
                           know where the matching .cfi_endproc goes, and
                           x86-64's .eh_frame FDE needs the function's
                           total instruction-byte length between the two */
    int is_callee_save_push; /* only for OP_PUSH/OP_POP: true when this
                           push/pop is one of wrap_function_body's
                           compiler-synthesized callee-save saves/
                           restores, false for a user-written 'push SRC;'/
                           'pop DST;' statement. CFI must only describe
                           the former -- a register wrap_function_body
                           protects genuinely lives at a fixed CFA-
                           relative slot for the rest of the function, but
                           a user's own mid-function push is a transient
                           scratch use the unwinder has no fixed
                           expectation about (and would be actively wrong
                           to assume one for). */
    operand_t args[7];  /* only for OP_SYSCALL: [0]=syscall number, [1..6]=args */
    int nargs;          /* only for OP_SYSCALL: how many of args[] are set */
    operand_t base_reg; /* only for OP_ILOAD/OP_ISTORE/OP_HFIELD_LOAD/
                            OP_HFIELD_STORE: the rBASE in rBASE[rIDX] or
                            rBASE.field */
    operand_t idx_reg;  /* the rIDX in rBASE[rIDX] (OP_ILOAD/OP_ISTORE) or in
                            name[rIDX] (OP_LALOAD/OP_LASTORE) */
    int const_offset;   /* OP_HFIELD_LOAD/OP_HFIELD_STORE only: the
                            field's compile-time byte offset from
                            base_reg, added unscaled (see the
                            OP_HFIELD_LOAD comment on opcode_t for why
                            this isn't idx_reg*elem_size like OP_ILOAD) */
    int elem_size;      /* element width in bytes (1/2/4/8) for
                            OP_ILOAD/OP_ISTORE/OP_LALOAD/OP_LASTORE. For the
                            local-array ops this is validated at parse time
                            against the array's own declared element size
                            (see the laloadN/lastoreN parsing), so codegen
                            can trust it matches without re-checking. For
                            OP_LALOAD, the array itself is described by src
                            (kind==OPND_LOCAL, local_offset/frames_up
                            pointing at element 0 -- see declare_local_array)
                            and the loaded value's destination register is
                            dst, same src/dst roles OP_LOAD already uses.
                            OP_LASTORE mirrors this with the roles swapped:
                            src is the register being written, dst
                            (OPND_LOCAL) is the array. */
    int frame_bytes;    /* only for OP_FRAME_OPEN/OP_FRAME_CLOSE: bytes of
                            local-variable stack space for this block,
                            back-patched onto OP_FRAME_OPEN once the
                            block's '}' is reached (see declare__local) */
    int xaddr_scale;     /* OP_XLOAD/OP_XSTORE only: the compile-time
                            constant SCALE in rBASE[rIDX*SCALE+DISP],
                            always 1/2/4/8 (validated at parse time --
                            see parse__xaddr). Deliberately a separate
                            field from elem_size rather than reusing it:
                            elem_size means "load/store width in bytes"
                            on every other opcode that has it (OP_ILOAD/
                            OP_ISTORE/OP_LALOAD/OP_LASTORE), and
                            OP_XLOAD/OP_XSTORE need both a width (how
                            many bytes to move) AND an independently-
                            chosen scale (how rIDX is multiplied) at the
                            same time -- e.g. 'xload8 rBASE[rIDX*4]'
                            loads a single byte at a stride of 4, so
                            elem_size=1 and xaddr_scale=4 must coexist.
                            const_offset (already declared above) doubles
                            as DISP for these two opcodes, the same
                            const_offset OP_HFIELD_LOAD/OP_HFIELD_STORE
                            use for their own unscaled displacement. */
    /* Atomics (OP_ATOMIC_*, OP_FENCE). 'dst' (already declared above) is
       reused for the memory location (OPND_SYM or OPND_LOCAL) being
       operated on, matching the role it plays for OP_STORE -- 'the place
       being written'. 'src' (already declared above) is reused for the
       value operand on ADD/SUB/AND/OR/XOR/SWAP (register or immediate).
       result_reg is where the old value (or, for CAS, the 1/0 success
       flag) is written -- a separate field rather than overloading src/dst
       again since atomics are the one place Chard has three meaningfully
       different operands (target, value-in, value-out) at once. */
    operand_t result_reg;  /* OP_ATOMIC_*: destination register for the
                               returned old value (or CAS success flag).
                               OP_FMA: reused (unrelated to any atomic
                               meaning) as fB, the second multiplicand --
                               see cas_expected/cas_desired below for fA/
                               fC and the OP_FMA opcode_t comment for why
                               fma borrows this trio instead of adding a
                               fourth pair of dedicated fields. */
    operand_t cas_expected; /* OP_ATOMIC_CAS only: register holding the
                                expected old value to compare against.
                                OP_FMA: reused as fA, the first
                                multiplicand (fDST = fA*fB + fC; fDST
                                itself is dst, as with every other float
                                op). */
    operand_t cas_desired;  /* OP_ATOMIC_CAS only: register holding the
                                value to store if the comparison succeeds.
                                OP_FMA: reused as fC, the addend. */
    int atomic_width;      /* OP_ATOMIC_*: unused at parse time -- codegen
                               computes this itself via operand_mem_size(),
                               the same helper OP_LOAD/OP_STORE already use
                               to infer a symbol/local's declared width, so
                               there's exactly one place that inference
                               logic lives */
    mem_order_t mem_order;  /* OP_ATOMIC_*, OP_FENCE: requested memory
                                ordering, from an optional '%relaxed' /
                                '%acquire' / '%release' / '%acq_rel' /
                                '%seq_cst' suffix (MEM_ORDER_SEQ_CST, the
                                strongest, if the suffix is omitted --
                                see parse_instr_line's atomic-suffix
                                handling and mem_order_t's own comment
                                for why seq_cst is the default rather
                                than relaxed). x86-64 codegen ignores
                                this value entirely: every 'lock'-
                                prefixed RMW is already full-barrier-
                                strength in hardware, there is no cheaper
                                x86 form to opt into regardless of what's
                                requested here. AArch64/RISC-V codegen
                                select between plain and acquire/release/
                                exclusive-access instruction variants
                                based on it (ldxr vs ldaxr, stxr vs
                                stlxr; amo*.aq/.rl bits) -- see each
                                backend's OP_ATOMIC_* / OP_FENCE case. */
    operand_t len_reg;     /* OP_I2S/OP_S2I: the rLEN register. For i2s
                               (i2s rSRC > rBUF, rLEN) this is an output --
                               src/dst already hold rSRC/rBUF, so len_reg
                               is the one remaining operand. For s2i
                               (s2i rBUF, rLEN > rDST) this is an input --
                               dst holds rBUF and result_reg holds rDST
                               (reusing the same "third register" field
                               OP_ATOMIC_CAS already uses result_reg for),
                               so len_reg is again the one field left
                               over. Kept as one shared field, rather than
                               two named ilen_reg/slen_reg, since the two
                               ops never coexist in the same instr_t. */
    char raw_text[MAX_STRLEN]; /* only for OP_RAW: the string literal's
                                   contents (escapes already resolved,
                                   same resolution pass 'ascii' decls
                                   use), emitted verbatim with a single
                                   leading indent and no further
                                   processing -- see each backend's
                                   OP_RAW case. Same target text is
                                   emitted on all three backends since
                                   the string is opaque to Chard; if the
                                   raw instruction is target-specific,
                                   that's the caller's responsibility
                                   (see the 'raw' parsing comment). */
    int raw_data_size;          /* only for OP_RAWDATA: element width in
                                    bytes (1/2/4/8 for int, or 4/8 for
                                    float, i.e. f32/f64), from the iK/fK
                                    size specifier -- same db/dw/dd/dq
                                    choice 'data'/'volatile' declarations
                                    use */
    int raw_data_is_float;      /* only for OP_RAWDATA: set when the size
                                    specifier was fK rather than iK --
                                    selects raw_data_fvals[] instead of
                                    raw_data_vals[], and emits each value
                                    through float__bits/double__bits as
                                    dd/dq (or .word/.xword,
                                    .word/.dword on the other two
                                    backends) the same way a 'data fK'
                                    array's initializer does, rather than
                                    as a plain decimal db/dw/dd/dq like
                                    the integer case. */
    long *raw_data_vals; /* only for OP_RAWDATA when !raw_data_is_float:
                                    the literal integer values, in source
                                    order. Grown on demand -- see
                                    raw_data_vals_cap. */
    int raw_data_vals_cap;
    double *raw_data_fvals; /* only for OP_RAWDATA when raw_data_is_float:
                                    the literal float values, in source
                                    order, mirroring raw_data_vals above.
                                    Grown on demand -- see
                                    raw_data_fvals_cap. */
    int raw_data_fvals_cap;
    int raw_data_nvals;         /* only for OP_RAWDATA: how many of
                                    raw_data_vals[]/raw_data_fvals[] are
                                    set */
    int *raw_data_val_is_label;    /* only for OP_RAWDATA + !raw_data_is_float:
                                    parallel to raw_data_vals, same
                                    '&label' jump-table-entry mechanism as
                                    decl_t's data_val_is_label -- see its
                                    comment. NULL when no element is a
                                    label. Same cap as raw_data_vals
                                    (raw_data_vals_cap). */
    char **raw_data_val_labels;    /* only for OP_RAWDATA + !raw_data_is_float:
                                    parallel to raw_data_vals, label text
                                    where raw_data_val_is_label[v] is set
                                    -- see decl_t's data_val_labels. Same
                                    cap as raw_data_vals
                                    (raw_data_vals_cap). */
    opcode_t assert_jmp_op;     /* only for OP_ASSERT: the *inverted*
                                    conditional-jump opcode (OP_JE/OP_JNE/
                                    etc, from invert_cond_op -- same
                                    inversion if/while already use to
                                    skip their body) that codegen emits
                                    right before the trap. Taking that
                                    jump means the asserted condition
                                    was false. */
    int src_line;                /* source line this instruction came from
                                    (g_line_no at push__instr time), 0 for
                                    instructions synthesized after parsing
                                    (e.g. splice_global_pin_loads' own pin
                                    loads). Exists purely so a post-parse
                                    pass -- currently check_global_pin_
                                    violations -- can still point at a
                                    real line number instead of only an
                                    instruction index, the same way every
                                    parse-time fail()/failf() already
                                    does via g_line_no. */
    const char *src_file;         /* g_filename at push__instr time -- same
                                    reasoning as src_line, split into its
                                    own field once source could span
                                    multiple files (| include/| data): a
                                    line number alone no longer identifies
                                    a line without also saying which file
                                    it's in. NULL for synthesized
                                    instructions, same convention as
                                    src_line's 0. */
    int load_signed;              /* OP_LOAD/OP_ILOAD only: 1 if this load
                                    should sign-extend a sub-64-bit value
                                    into its full-width destination
                                    register, 0 (the default, so existing
                                    zero-initialized instr_t values and
                                    every pre-existing call site keep their
                                    original zero-extending behavior with
                                    no source change needed) for
                                    zero-extend. Set by the 's' width-
                                    suffix variant of loadN/iloadN (e.g.
                                    'load32s', 'iload16s') or by a
                                    'signed'-declared symbol for plain
                                    unsuffixed 'load SYM > rX;' -- see the
                                    parse sites for each. Irrelevant (left
                                    0) for 64-bit widths, which fill the
                                    whole register either way and have no
                                    extension to speak of, and for every
                                    other opcode, which doesn't touch this
                                    field at all. */
} instr_t;
typedef struct {
    int reg_num;              /* 1..12; sp cannot be pinned */
    char sym[MAX_SYMLEN];
    int is_addr;               /* 0: value pin ('= SYM'), rN preloaded with
                                   SYM's initial value via OP_LOAD.
                                   1: address pin ('= &SYM'), rN preloaded
                                   with &SYM via OP_LEA -- see the comment
                                   block above for why dereferencing an
                                   address pin always stays live while a
                                   value pin is a frozen snapshot. */
} global_pin_t;
#define MAX_GLOBAL_PINS 12
#define MAX_PARAMS 12 /* matches r1-r12, Chard's full general-register range */
typedef struct {
    char names[MAX_PARAMS][MAX_SYMLEN];
    int nparams;
} param_scope_t;
typedef struct {
    char name[MAX_SYMLEN];
    int nparams;
    int ret_reg; /* 1-based, matching r1-r12 */
} func_sig_t;
#define MAX_LIBC_ARGS 6 /* every target's C ABI passes at most 6 integer/
                            pointer args in registers before spilling to
                            the stack; Chard v1 doesn't support the stack-
                            spill case, matching how syscall() is also
                            capped at register-only args (6, coincidentally
                            the same number, though for the unrelated
                            reason that Linux syscalls take at most 6
                            args) */
typedef struct {
    char name[MAX_SYMLEN];
    int nargs;
    char lib[MAX_SYMLEN]; /* optional -- '\0' means "plain libc symbol, no extra
        -l needed" (the historical default, still the common case: printf,
        malloc, exit, etc all come for free with any C runtime link). Set
        when the extern was written with a trailing 'lib "name";' tag,
        meaning the symbol lives in some other library the final link step
        has to be told about with '-lname' -- see the "extern library
        tagging" block comment above the 'extern' parse site. */
} extern_sig_t;
#define MAX_COLLECTED_ERRORS 50
typedef char macro_line_t[MAX_LINE]; /* also used by macro_def_t's body
                                          array further below */
typedef struct {
    char name[MAX_SYMLEN];
    char params[MAX_MACRO_PARAMS][MAX_SYMLEN];
    int nparams;
    macro_line_t *body; /* grown on demand -- see DA_ENSURE */
    int nbody;
    int body_cap;
} macro_def_t;
typedef struct {
    char name[MAX_SYMLEN];
    long value;
} equ_def_t;
typedef struct {
    char name[MAX_SYMLEN];
    char regtext[8]; /* the literal register spelling this expands to,
                         e.g. "r7", "sp", "f3" -- always short enough
                         that a fixed small buffer is simpler than
                         reusing operand_t just to hold three
                         characters' worth of substitution text */
} alias_def_t;
typedef struct {
    char qualified_name[2 * MAX_SYMLEN]; /* "EnumName.MEMBER", the exact
                                             token text a reference must
                                             match whole */
    long value;
} enum_member_t;
typedef struct {
    char name[MAX_SYMLEN];
    int offset;       /* byte offset from the struct's base */
    int size_bytes;   /* 1/2/4/8 */
    int is_float;
} struct_field_t;
typedef struct {
    char name[MAX_SYMLEN];
    struct_field_t *fields; /* grown on demand -- see DA_ENSURE */
    int nfields;
    int fields_cap;
    int total_size;   /* sum of all fields' size_bytes, tight-packed */
} struct_def_t;
typedef struct { const char *p; jmp_buf *err; } expr_parser_t;
typedef struct {
    int frame_size;        /* running total of bytes reserved so far */
    int prologue_instr_idx; /* index into prog[] of this block's OP_FRAME_ADJ */
    int first_decl_idx;     /* index into decls[] where this block's locals start */
    int is_function_root;   /* 1 if this frame is any '@name: { ... }' or
                                '@name(...) -> rN: { ... }' block's
                                outermost block -- its own '}' (not any
                                nested block's) is what ends the active
                                parameter scope (see the "Function
                                parameters" section) and triggers
                                callee-save wrapping + CFI emission below,
                                whether or not it declared a signature */
    func_sig_t *func_sig;    /* set only when is_function_root AND the
                                 block declared '(params) -> rN'; NULL for
                                 a plain callable '@name: { ... }'
                                 subroutine with no params/return
                                 register. wrap_function_body and CFI
                                 emission both treat NULL as nparams=0/
                                 ret_reg=0 rather than skipping the frame */
    int label_instr_idx;    /* only meaningful when is_function_root: the
                                 prog[] index of this function's own
                                 OP_LABEL, so close_local_frame can
                                 back-patch that label's
                                 func_frame_close_idx once the matching
                                 OP_FRAME_CLOSE index is known (mirrors
                                 how prologue_instr_idx's frame_bytes gets
                                 back-patched at the same point) */
    int *spill_is_local; /* stack discipline for 'spill'/'unspill'
                                        (see that section): spill_is_local[i]
                                        is 0 if the i-th still-live spill in
                                        this block set aside a register
                                        (spill_regs[i] names which one), or 1
                                        if it set aside a local's own value
                                        (spill_local_decl_idx[i] names which
                                        local, spill_regs[i] unused). 'unspill'
                                        must name whatever is on top of this
                                        stack -- last spilled, first
                                        unspilled, checked at parse time.
                                        Registers and locals share this one
                                        stack (a spilled local can sit above
                                        a spilled register or vice versa).
                                        Grown on demand alongside the three
                                        arrays below -- see spill_cap and
                                        DA_ENSURE -- so a block can have any
                                        number of outstanding spills. */
    int *spill_regs; /* spill_regs[i]: which register the i-th
                                    still-live spill set aside, when
                                    spill_is_local[i] is 0 (unused otherwise) */
    int *spill_local_decl_idx; /* spill_local_decl_idx[i]: the
                                              decls[] index of the local
                                              whose value the i-th still-live
                                              spill set aside, when
                                              spill_is_local[i] is 1 (unused
                                              otherwise) -- needed the same
                                              way spill_decl_idx below is,
                                              to know which local's name and
                                              storage to restore into on
                                              unspill */
    int *spill_decl_idx; /* spill_decl_idx[i] is the decls[]
                                        index of the hidden __spillN local
                                        backing the i-th still-live spill's
                                        saved value (register or local value
                                        alike). Needed because declare__local
                                        never removes an individual local
                                        from decls[] on unspill (only a whole
                                        block's close reclaims decls[] in
                                        bulk -- see close_local_frame), so
                                        "the most recently declared __spillN"
                                        is NOT reliably the same as "the slot
                                        this particular unspill needs" once
                                        more than one spill/unspill cycle has
                                        happened in the same block; tracking
                                        the exact index sidesteps that. */
    int nspilled;
    int spill_cap; /* shared capacity for the four spill_* arrays above --
                       see DA_ENSURE call sites, which grow all four in
                       lockstep so they stay the same length */
} local_frame_t;
#define MAX_LOCAL_FRAME_DEPTH 32 /* @label blocks may nest arbitrarily (each
                                     gets its own frame-pointer-based frame,
                                     see above); matches MAX_SCOPE_DEPTH, the
                                     equivalent limit for if/while nesting */
typedef struct { const char *name; opcode_t op; } opmap_t;
typedef enum { SCOPE_IF, SCOPE_WHILE, SCOPE_FOR } scope_kind_t;
typedef struct {
    scope_kind_t kind;
    char end_label[MAX_SYMLEN];   /* if: falls through here; while/for: loop exit
                                      (also 'break' target for while/for) */
    char else_label[MAX_SYMLEN];  /* if only: where a false condition jumps to */
    char top_label[MAX_SYMLEN];   /* while/for: loop head, for the backward jmp --
                                      for while this doubles as the 'continue'
                                      target (re-testing the condition is all
                                      continue needs to do); for 'for' it is NOT
                                      the continue target, since continue must
                                      still run the increment before re-testing
                                      -- see continue_label below */
    char continue_label[MAX_SYMLEN]; /* for only: where 'continue' jumps -- the
                                      increment step, placed right before the
                                      condition re-test. Unused (empty) for
                                      SCOPE_WHILE, which reuses top_label instead. */
    int has_else;                 /* if only: whether 'else' has been seen yet */
    int pending_close;            /* if only: '}' seen, waiting to see if 'else' follows */
    operand_t for_var;             /* for only: loop variable (always a register) */
    operand_t for_step;            /* for only: step operand (reg or imm), added
                                      to for_var each iteration */
} scope_t;
#define MAX_SCOPE_DEPTH 32
typedef struct {
    const char *name;
    long x86_64;
    long generic; /* shared by AArch64 and RISC-V */
} syscall_name_t;
static const syscall_name_t syscall_names[] = {
    /* file I/O */
    { "read",             0,   63 },
    { "write",            1,   64 },
    { "close",            3,   57 },
    { "fstat",            5,   80 },
    { "lseek",            8,   62 },
    { "pread64",         17,   67 },
    { "pwrite64",        18,   68 },
    { "readv",           19,   65 },
    { "writev",          20,   66 },
    { "pipe2",          293,   59 },
    { "sendfile",        40,   71 },
    { "getcwd",          79,   17 },
    { "chdir",           80,   49 },
    { "fchdir",          81,   50 },
    { "fchmod",          91,   52 },
    { "fchown",          93,   55 },
    { "umask",           95,  166 },
    { "flock",           73,   32 },
    { "fsync",           74,   82 },
    { "fdatasync",       75,   83 },
    { "truncate",        76,   45 },
    { "ftruncate",       77,   46 },
    { "fcntl",           72,   25 },
    { "ioctl",           16,   29 },
    { "getdents64",     217,   61 },
    { "openat",         257,   56 },
    { "mkdirat",        258,   34 },
    { "mknodat",        259,   33 },
    { "fchownat",       260,   54 },
    { "newfstatat",     262,   79 },  /* generic spells this "fstatat" */
    { "unlinkat",       263,   35 },
    { "renameat",       264,   38 },
    { "renameat2",      316,  276 },
    { "linkat",         265,   37 },
    { "symlinkat",      266,   36 },
    { "readlinkat",     267,   78 },
    { "fchmodat",       268,   53 },
    { "faccessat",      269,   48 },
    { "utimensat",      280,   88 },
    { "statx",          332,  291 },
    { "sync",           162,   81 },
    { "syncfs",         306,  267 },
    { "sync_file_range",277,   84 },
    { "readahead",      187,  213 },
    { "fallocate",      285,   47 },
    { "splice",         275,   76 },
    { "tee",            276,   77 },
    { "vmsplice",       278,   75 },
    { "copy_file_range",326,  285 },
    { "preadv",         295,   69 },
    { "pwritev",        296,   70 },
    { "preadv2",        327,  286 },
    { "pwritev2",       328,  287 },
    { "name_to_handle_at",303,264 },
    { "open_by_handle_at",304,265 },
    { "statfs",         137,   43 },
    { "fstatfs",        138,   44 },
    /* filesystem admin */
    { "chroot",         161,   51 },
    { "mount",          165,   40 },
    { "umount2",        166,   39 },
    { "pivot_root",     155,   41 },
    { "swapon",         167,  224 },
    { "swapoff",        168,  225 },
    { "quotactl",       179,   60 },
    { "quotactl_fd",    443,  443 },
    { "acct",           163,   89 },
    /* xattr */
    { "setxattr",       188,    5 },
    { "lsetxattr",      189,    6 },
    { "fsetxattr",      190,    7 },
    { "getxattr",       191,    8 },
    { "lgetxattr",      192,    9 },
    { "fgetxattr",      193,   10 },
    { "listxattr",      194,   11 },
    { "llistxattr",     195,   12 },
    { "flistxattr",     196,   13 },
    { "removexattr",    197,   14 },
    { "lremovexattr",   198,   15 },
    { "fremovexattr",   199,   16 },
    /* directory watching / event fds / io multiplexing */
    { "inotify_init1",  294,   26 },
    { "inotify_add_watch",254,  27 },
    { "inotify_rm_watch",255,  28 },
    { "epoll_create1",  291,   20 },
    { "epoll_ctl",      233,   21 },
    { "epoll_pwait",    281,   22 },
    { "epoll_pwait2",   441,  441 },
    { "eventfd2",       290,   19 },
    { "signalfd4",      289,   74 },
    { "timerfd_create", 283,   85 },
    { "timerfd_settime",286,   86 },
    { "timerfd_gettime",287,   87 },
    { "pselect6",       270,   72 },
    { "ppoll",          271,   73 },
    { "lookup_dcookie", 212,   18 },  /* number exists on both; kernel-side
                                          it's a permanent stub either way */
    { "fanotify_init",  300,  262 },
    { "fanotify_mark",  301,  263 },
    /* process / misc */
    { "exit",            60,   93 },
    { "exit_group",     231,   94 },
    { "getpid",          39,  172 },
    { "getppid",        110,  173 },
    { "gettid",         186,  178 },
    { "nanosleep",       35,  101 },
    { "clone",           56,  220 },
    { "clone3",         435,  435 },
    { "execve",          59,  221 },
    { "execveat",       322,  281 },
    { "wait4",           61,  260 },
    { "waitid",         247,   95 },
    { "kill",            62,  129 },
    { "tkill",          200,  130 },
    { "tgkill",         234,  131 },
    { "dup",             32,   23 },
    { "dup3",           292,   24 },
    { "ptrace",         101,  117 },
    { "personality",    135,   92 },
    { "prctl",          157,  167 },
    { "seccomp",        317,  277 },
    { "getrandom",      318,  278 },
    { "capget",         125,   90 },
    { "capset",         126,   91 },
    { "restart_syscall",219,  128 },
    { "set_tid_address",218,   96 },
    { "unshare",        272,   97 },
    { "prlimit64",      302,  261 },
    { "getrlimit",       97,  163 },
    { "setrlimit",      160,  164 },
    { "getrusage",       98,  165 },
    { "sysinfo",         99,  179 },
    { "times",          100,  153 },
    { "syslog",         103,  116 },
    /* users / groups / sessions */
    { "getuid",         102,  174 },
    { "geteuid",        107,  175 },
    { "setuid",         105,  146 },
    { "getgid",         104,  176 },
    { "getegid",        108,  177 },
    { "setgid",         106,  144 },
    { "setreuid",       113,  145 },
    { "setregid",       114,  143 },
    { "setresuid",      117,  147 },
    { "getresuid",      118,  148 },
    { "setresgid",      119,  149 },
    { "getresgid",      120,  150 },
    { "setfsuid",       122,  151 },
    { "setfsgid",       123,  152 },
    { "getgroups",      115,  158 },
    { "setgroups",      116,  159 },
    { "setpgid",        109,  154 },
    { "getpgid",        121,  155 },
    { "setsid",         112,  157 },
    { "getsid",         124,  156 },
    { "uname",           63,  160 },
    { "sethostname",    170,  161 },
    { "setdomainname",  171,  162 },
    /* signals */
    { "rt_sigaction",    13,  134 },
    { "rt_sigprocmask",  14,  135 },
    { "rt_sigreturn",    15,  139 },
    { "rt_sigpending",  127,  136 },
    { "rt_sigtimedwait",128,  137 },
    { "rt_sigqueueinfo",129,  138 },
    { "rt_sigsuspend",  130,  133 },
    { "rt_tgsigqueueinfo",297,240 },
    { "sigaltstack",    131,  132 },
    /* timers / clocks */
    { "gettimeofday",    96,  169 },
    { "settimeofday",   164,  170 },
    { "adjtimex",       159,  171 },
    { "clock_adjtime",  305,  266 },
    { "getitimer",       36,  102 },
    { "setitimer",       38,  103 },
    { "timer_create",   222,  107 },
    { "timer_settime",  223,  110 },
    { "timer_gettime",  224,  108 },
    { "timer_getoverrun",225, 109 },
    { "timer_delete",   226,  111 },
    { "clock_settime",  227,  112 },
    { "clock_gettime",  228,  113 },
    { "clock_getres",   229,  114 },
    { "clock_nanosleep",230,  115 },
    /* scheduling / priority / affinity */
    { "sched_yield",     24,  124 },
    { "sched_setparam", 142,  118 },
    { "sched_getparam", 143,  121 },
    { "sched_setscheduler",144,119 },
    { "sched_getscheduler",145,120 },
    { "sched_get_priority_max",146,125 },
    { "sched_get_priority_min",147,126 },
    { "sched_rr_get_interval",148,127 },
    { "sched_setaffinity",203,122 },
    { "sched_getaffinity",204,123 },
    { "sched_setattr",  314,  274 },
    { "sched_getattr",  315,  275 },
    { "setpriority",    141,  140 },
    { "getpriority",    140,  141 },
    { "ioprio_set",     251,   30 },
    { "ioprio_get",     252,   31 },
    { "getcpu",         309,  168 },
    /* futex / robust lists */
    { "futex",          202,   98 },
    { "futex_waitv",    449,  449 },
    { "set_robust_list",273,   99 },
    { "get_robust_list",274,  100 },
    /* modules / kexec / reboot */
    { "init_module",    175,  105 },
    { "finit_module",   313,  273 },
    { "delete_module",  176,  106 },
    { "kexec_load",     246,  104 },
    { "reboot",         169,  142 },
    { "vhangup",        153,   58 },
    /* memory */
    { "mmap",             9,  222 },
    { "mprotect",        10,  226 },
    { "munmap",          11,  215 },
    { "brk",             12,  214 },
    { "mremap",          25,  216 },
    { "msync",           26,  227 },
    { "mincore",         27,  232 },
    { "madvise",         28,  233 },
    { "mlock",          149,  228 },
    { "munlock",        150,  229 },
    { "mlockall",       151,  230 },
    { "munlockall",     152,  231 },
    { "mlock2",         325,  284 },
    { "mbind",          237,  235 },
    { "set_mempolicy",  238,  237 },
    { "get_mempolicy",  239,  236 },
    { "set_mempolicy_home_node",450,450 },
    { "migrate_pages",  256,  238 },
    { "move_pages",     279,  239 },
    { "remap_file_pages",216, 234 },
    { "fadvise64",      221,  223 },
    { "pkey_mprotect",  329,  288 },
    { "pkey_alloc",     330,  289 },
    { "pkey_free",      331,  290 },
    { "memfd_create",   319,  279 },
    { "userfaultfd",    323,  282 },
    { "membarrier",     324,  283 },
    { "cachestat",      451,  451 },
    { "map_shadow_stack",453, 453 },
    { "process_vm_readv",310,270 },
    { "process_vm_writev",311,271 },
    { "process_madvise",440,  440 },
    { "process_mrelease",448,448 },
    /* IPC: SysV shm/sem/msg queues */
    { "shmget",          29,  194 },
    { "shmat",           30,  196 },
    { "shmctl",          31,  195 },
    { "shmdt",           67,  197 },
    { "semget",          64,  190 },
    { "semop",           65,  193 },
    { "semctl",          66,  191 },
    { "semtimedop",     220,  192 },
    { "msgget",          68,  186 },
    { "msgsnd",          69,  189 },
    { "msgrcv",          70,  188 },
    { "msgctl",          71,  187 },
    /* IPC: POSIX message queues */
    { "mq_open",        240,  180 },
    { "mq_unlink",      241,  181 },
    { "mq_timedsend",   242,  182 },
    { "mq_timedreceive",243,  183 },
    { "mq_notify",      244,  184 },
    { "mq_getsetattr",  245,  185 },
    /* IPC: async I/O (AIO) */
    { "io_setup",       206,    0 },
    { "io_destroy",     207,    1 },
    { "io_submit",      209,    2 },
    { "io_cancel",      210,    3 },
    { "io_getevents",   208,    4 },
    /* keys */
    { "add_key",        248,  217 },
    { "request_key",    249,  218 },
    { "keyctl",         250,  219 },
    /* landlock */
    { "landlock_create_ruleset",444,444 },
    { "landlock_add_rule",445, 445 },
    { "landlock_restrict_self",446,446 },
    /* perf / tracing / bpf */
    { "perf_event_open",298,  241 },
    { "bpf",            321,  280 },
    { "kcmp",           312,  272 },
    /* pidfd */
    { "pidfd_open",     434,  434 },
    { "pidfd_getfd",    438,  438 },
    { "pidfd_send_signal",424,424 },
    /* io_uring */
    { "io_uring_setup", 425,  425 },
    { "io_uring_enter", 426,  426 },
    { "io_uring_register",427,427 },
    /* new mount API */
    { "open_tree",      428,  428 },
    { "move_mount",     429,  429 },
    { "fsopen",         430,  430 },
    { "fsconfig",       431,  431 },
    { "fsmount",        432,  432 },
    { "fspick",         433,  433 },
    { "mount_setattr",  442,  442 },
    { "close_range",    436,  436 },
    { "openat2",        437,  437 },
    { "faccessat2",     439,  439 },
    { "fchmodat2",      452,  452 },
    { "rseq",           334,  293 },
    /* BSD sockets */
    { "socket",          41,  198 },
    { "connect",         42,  203 },
    { "accept",          43,  202 },
    { "accept4",        288,  242 },
    { "sendto",          44,  206 },
    { "recvfrom",        45,  207 },
    { "sendmsg",         46,  211 },
    { "recvmsg",         47,  212 },
    { "sendmmsg",       307,  269 },
    { "recvmmsg",       299,  243 },
    { "shutdown",        48,  210 },
    { "bind",            49,  200 },
    { "listen",          50,  201 },
    { "getsockname",     51,  204 },
    { "getpeername",     52,  205 },
    { "socketpair",      53,  199 },
    { "setsockopt",      54,  208 },
    { "getsockopt",      55,  209 },
    { "setns",          308,  268 },
};
#define NUM_SYSCALL_NAMES (int)(sizeof(syscall_names) / sizeof(syscall_names[0]))
typedef struct {
    char text[MAX_LINE];
    int orig_line_no;
    const char *filename; /* which file this line came from -- points
                              into file_names[] (never freed/reused).
                              Set from g_filename at collection time, so
                              included files and macro expansions report
                              their true origin in error messages. */
} pp_line_t;
#define MAX_INCLUDE_DEPTH  32
#define MAX_PATH_LEN        512
typedef struct {
    char name[MAX_SYMLEN];
    long next_value;
} enum_collect_t;
typedef struct {
    struct_def_t *def; /* points into struct_defs[]; already reserved
                           (nstruct_defs incremented) when this is set,
                           so a malformed body still leaves a real
                           (if incomplete) entry rather than a dangling
                           pointer -- matches how a %macro with a
                           missing %endmacro is caught by EOF instead
                           of ever being left half-registered anywhere
                           else in the file. */
} struct_collect_t;
typedef struct {
    const char *regs[13];          /* [0] unused, [1..12] = r1..r12, 64-bit-or-native width */
    const char *const *regs_32;    /* NULL if the target has no separate sub-word name (see note) */
    const char *const *regs_16;    /* (only x86-64 needs these; AArch64/RISC-V leave them NULL) */
    const char *const *regs_8;
    const char *sp;                 /* name of the stack pointer */
    const char *fp;                 /* name of the dedicated frame-pointer
                                        register for @label: { } blocks --
                                        each target's own ABI-conventional
                                        choice (rbp / x29 / s0), unused
                                        elsewhere in Chard's r1-r12 mapping
                                        or scratch register */
    const char *scratch;            /* primary scratch register, not in regs[1..12] */
    const char *imm_prefix;         /* e.g. "#" on AArch64 immediates, "" elsewhere */
    int stack_slot_bytes;           /* bytes sp moves per push/pop (see rationale below) */
    const char *fregs[8];           /* [0] unused, [1..7] = f1..f7 (user-visible);
                                        f7 doubles as float-immediate-seed scratch
                                        (see parse__decl's float local-initializer
                                        path) so it's never handed out beyond
                                        that -- kept in this array anyway,
                                        rather than excluded, so fregs[] stays
                                        a uniform 1:1 index-to-fN mapping like
                                        regs[] above; f8, this target's
                                        dedicated float codegen scratch (the
                                        float equivalent of 'scratch' above),
                                        is named separately below since (like
                                        'scratch') it's never user-addressable
                                        as an operand */
    const char *fscratch;           /* dedicated float scratch register, not
                                        in fregs[1..7] and not f7 either --
                                        used internally by codegen (e.g. to
                                        hold a converted/narrowed intermediate
                                        value) the same way 'scratch' is used
                                        for integers */
    const char *sregs[8];           /* [0] unused, [1..7] = s1..s7 (user-
                                        visible, single-precision f32 --
                                        see operand_t.is_f32), same shape
                                        and same "index 7 doubles as
                                        immediate-seed scratch" convention
                                        as fregs[] above, just for the f32
                                        file instead of f64. On x86-64 and
                                        AArch64 these are DIFFERENT
                                        register names than fregs[] (the
                                        32-bit view of the same physical
                                        SIMD/FP register: xmm0 stays "xmm0"
                                        on x86 regardless -- x86's xmm
                                        registers don't have distinct
                                        32-vs-64-bit *names*, the width
                                        lives in the instruction mnemonic
                                        (addss vs addsd) instead, so
                                        sregs[] and fregs[] are
                                        byte-identical on x86-64 -- but
                                        AArch64 does name them differently:
                                        s0 (32-bit) vs d0 (64-bit) are
                                        distinct register names for the
                                        same physical v0. RISC-V is like
                                        x86: fa0 is fa0 regardless of
                                        width, the .s/.d suffix on the
                                        instruction carries the
                                        distinction, so sregs[]==fregs[]
                                        there too. See each backend's
                                        float codegen for how the
                                        mnemonic-vs-register-name split is
                                        actually applied per target. */
    const char *sscratch;           /* f32 equivalent of fscratch, same
                                        "index 8, never user-addressable"
                                        convention */
} target_def_t;
static const char *const x86_64_regs_32[13] = {
    NULL, "eax", "ebx", "ecx", "edx", "esi", "edi",
    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d"
};
static const char *const x86_64_regs_16[13] = {
    NULL, "ax", "bx", "cx", "dx", "si", "di",
    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w"
};
static const char *const x86_64_regs_8[13] = {
    NULL, "al", "bl", "cl", "dl", "sil", "dil",
    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b"
};
static const target_def_t target_defs[3] = {
    [TARGET_X86_64] = {
        .regs = { NULL, "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
                  "r8", "r9", "r10", "r11", "r12", "r13" },
        .regs_32 = x86_64_regs_32,
        .regs_16 = x86_64_regs_16,
        .regs_8  = x86_64_regs_8,
        .sp = "rsp",
        .fp = "rbp",
        .scratch = "r14",    /* not in the r1-r12 mapping */
        .imm_prefix = "",
        .stack_slot_bytes = 8,
        /* SSE2 xmm0-xmm6 for f1-f7 (movsd/addsd/etc all operate on the
           low 64 bits of an xmm register regardless of scalar width,
           which is exactly the "always compute at f64" model floats
           use here -- see the OP_FADD family comment). xmm7 is
           fscratch. */
        .fregs = { NULL, "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6" },
        .fscratch = "xmm7",
        /* x86-64's xmm registers have no separate 32-vs-64-bit *name* --
           addss/movss (32-bit) and addsd/movsd (64-bit) both operate on
           the same xmm0..xmm7, just reading/writing a different number
           of low bits. So sregs is byte-identical to fregs here; the
           s-file/f-file distinction is carried entirely by which
           mnemonic codegen picks (see the OP_FADD family x86-64 case),
           not by which register name it prints. */
        .sregs = { NULL, "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6" },
        .sscratch = "xmm7",
    },
    [TARGET_AARCH64] = {
        .regs = { NULL, "x0", "x1", "x2", "x3", "x4", "x5",
                  "x6", "x7", "x8", "x9", "x10", "x11" },
        .sp = "sp",
        .fp = "x29",
        .scratch = "x12",    /* not in the x0-x11 mapping */
        .imm_prefix = "#",
        .stack_slot_bytes = 16,
        /* d0-d6 (the 64-bit view of AArch64's v0-v6 SIMD/FP registers)
           for f1-f7; d7 is fscratch. */
        .fregs = { NULL, "d0", "d1", "d2", "d3", "d4", "d5", "d6" },
        .fscratch = "d7",
        /* Unlike x86-64/RISC-V, AArch64 genuinely names the 32-bit and
           64-bit views of the same physical v0-v7 SIMD/FP register
           differently: s0 (32-bit, single precision) vs d0 (64-bit,
           double precision). So sregs really is a distinct set of
           strings here, not an alias of fregs -- s0..s6 for s1..s7,
           s7 doubling as scratch exactly like fregs' d7 does. */
        .sregs = { NULL, "s0", "s1", "s2", "s3", "s4", "s5", "s6" },
        .sscratch = "s7",
    },
    [TARGET_RISCV] = {
        .regs = { NULL, "a0", "a1", "a2", "a3", "a4", "a5",
                  "a6", "a7", "s1", "s2", "s3", "s4" },
        .sp = "sp",
        .fp = "s0",
        .scratch = "t0",     /* SCRATCH_RV; a second scratch t1 (SCRATCH_RV2) is
                                 declared locally in emit__riscv where it's needed */
        .imm_prefix = "",
        .stack_slot_bytes = 16,
        /* fa0-fa6 (RVD calling-convention float argument registers)
           for f1-f7; ft0 (a caller-saved scratch float register,
           outside the fa0-fa7/fs0-fs11 argument/saved ranges) is
           fscratch. */
        .fregs = { NULL, "fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6" },
        .fscratch = "ft0",
        /* Same situation as x86-64: RISC-V's F/D extensions share one
           physical register file (f0-f31, ABI-named fa0-fa7/fs0-fs11/
           ft0-ft11 etc) between single- and double-precision -- there's
           no separate 32-bit-view register name the way AArch64 has
           s0 vs d0. The width lives in the instruction mnemonic's .s
           vs .d suffix (fadd.s vs fadd.d), so sregs is byte-identical
           to fregs here too. */
        .sregs = { NULL, "fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6" },
        .sscratch = "ft0",
    },
};

#endif /* CHARD_TYPES_H */

