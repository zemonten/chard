# chard
small register-based asm-ish language i've been working on compiles to nasm-syntax assembly for x86-64, aarch64, and riscv no deps besides a c compiler and whatever assembler/linker you want to use for the actual
target.

```
mv 5 > r1;
mv 10 > r2;
add r2 > r1;
exit(0);
```
see that example? see how it points to its destination? see???

not trying to be a real language, but atleast you don't need to write thousands of lines of assembly times X architectures and it's actually structured too.

## building

```
cd src
make
```

that's it just needs gcc (or change CC in the makefile) 
I'm genuinely wondering why you would try to manifest more information of this

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

(elf mode links libc, bare mode doesn't because its bare mode whattt)

## gimmicks

- 3 backends (x86-64 / aarch64 / riscv), pick with -target
- `| mode elf;` vs `| mode bare;` (bare is default, no libc, freestanding)
- functions: `@name(a, b) -> r1: { ... }`, call with `call name(a,b) -> rX;`
- if/else, while, for, cmp-based conditionals
- structs, enums, local arrays, volatile/bss/rodata/data sections
- macros (`%macro ... %endmacro`)
- equ (constants) and alias (named registers)
- global pins (lock a symbol to a register, compiler skins you if you clobber it)
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

quick example with a function, struct, enum, loop:

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

yes. I plan on updating this README soon its going to be cooperate 

## for more info on chard

I haven't made a full programing reference for chard yet. maybe when its core matures a bit is all

## license

bro js go to the LICENSE file and read it, it's GPLv3

