#include "../../../chard.h"

void push__instr(instr_t i) {
    DA_ENSURE(prog, prog_cap, nprog, instr_t);
    i.src_line = g_line_no;
    i.src_file = g_filename;
    prog[nprog++] = i;
}

void insert_instrs_at(int at, const instr_t *instrs, int n) {
    DA_ENSURE_N(prog, prog_cap, nprog + n, instr_t);
    memmove(&prog[at + n], &prog[at], (size_t)(nprog - at) * sizeof(instr_t));
    memcpy(&prog[at], instrs, (size_t)n * sizeof(instr_t));
    nprog += n;
}

void delete_instr_at(int at) {
    memmove(&prog[at], &prog[at + 1], (size_t)(nprog - at - 1) * sizeof(instr_t));
    nprog--;
}

