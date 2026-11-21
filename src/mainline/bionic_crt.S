.text
.global _start
.type _start, %function
_start:
    mov x0, sp
    mov x1, #0
    adrp x2, main
    add x2, x2, :lo12:main
    adrp x3, _bionic_structors
    add x3, x3, :lo12:_bionic_structors
    bl __libc_init
    brk #0

.section .rodata
.align 3
_bionic_structors:
    .quad 0
    .quad 0
    .quad 0
    .quad 0
