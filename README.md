# sorry, but youre not a sigma.
small register-based asm-ish language i've been working on compiles to nasm-syntax assembly for x86-64, aarch64, and riscv no deps besides a c compiler and whatever assembler/linker you want to use for the actual
target.

```
mv 5 > r1;
mv 10 > r2;
add r2 > r1;
exit(0);
```

not trying to be a real language, but atleast you don't need to write 50k lines of assembly times X architectures and it's actually structured too

## building

```
cd src
make
```

that's it just needs gcc (or change CC in the makefile) there's also a
single-file version of the whole thing (chard.c) if you'd rather just
drop one file into your own build somewhere instead of the whole src/ because bro choose one

## usage

```
chard -c input.ch [-o output.s] [-target=x86-64|aarch64|riscv]
```

defaults to your host arch if you don't pass -target. output is nasm
syntax so you still need to assemble+link it yourself

```
chard -c hello.ch -o hello.s
nasm -f elf64 hello.s -o hello.o
gcc hello.o -o hello
./hello
```

(elf mode links libc, bare mode doesn't because its fucking bare mode)

## what it actually has

- 3 backends (x86-64 / aarch64 / riscv), pick with -target
- `| mode elf;` vs `| mode bare;` (bare is default, no libc, freestanding)
- functions: `@name(a, b) -> r1: { ... }`, call with `call name(a,b) -> rX;`
- if/else, while, for, cmp-based conditionals
- structs, enums, local arrays, volatile/bss/rodata/data sections
- macros (`%macro ... %endmacro`)
- equ (constants) and alias (named registers)
- global pins (lock a symbol to a register, compiler yells if you clobber it)
- `| include` other files
- errors actually point at the line and show source, e.g:

```
error: 'exit()' is a kernel syscall wrapper and is not available in
'| mode bare;' (the default) -- add '| mode elf;' at the top of the
file to use it
  --> hello.ch:1
   |
 1 | exit(0);
   |
```

quick example with a function + struct + enum + loop:

```
| mode elf;

enum Color { RED, GREEN, BLUE };
struct Point { i64 x; i64 y; };

@add_two(a, b) -> r1: {
    mv a > r1;
    add b > r1;
    ret;
}

@main: {
    call add_two(3, 4) -> r5;
    mv Color.GREEN > r6;

    local i64 arr[5];
    mv 0 > r7;
    while r7 < 5 {
        store r7 > arr[r7];
        add 1 > r7;
    }
    exit(0);
}
```

## okay???

if you're coding in assembly then chard make your life 100000% easier. come little children come to me


## license

haven't picked one yet don't do anything weird with it, for now 

