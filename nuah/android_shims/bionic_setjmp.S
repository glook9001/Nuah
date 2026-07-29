/*
 * Android and glibc jmp_buf are not ABI-compatible on x86-64.  Resolve the
 * opaque Android address to a stable host buffer, then tail-call glibc so it
 * records Roblox's caller frame rather than a C wrapper frame.
 */

.text

.symver nuah_host___sigsetjmp, __sigsetjmp@GLIBC_2.2.5
.symver nuah_host_longjmp, longjmp@GLIBC_2.2.5

.globl setjmp
.type setjmp, @function
setjmp:
  call nuah_host_jmpbuf@PLT
  movq %rax, %rdi
  movl $1, %esi
  jmp nuah_host___sigsetjmp@PLT
.size setjmp, .-setjmp

.globl _setjmp
.type _setjmp, @function
_setjmp:
  call nuah_host_jmpbuf@PLT
  movq %rax, %rdi
  xorl %esi, %esi
  jmp nuah_host___sigsetjmp@PLT
.size _setjmp, .-_setjmp

.globl sigsetjmp
.type sigsetjmp, @function
sigsetjmp:
  pushq %rsi
  call nuah_host_jmpbuf@PLT
  popq %rsi
  movq %rax, %rdi
  jmp nuah_host___sigsetjmp@PLT
.size sigsetjmp, .-sigsetjmp

.macro NUAH_LONGJMP name
  .globl \name
  .type \name, @function
\name:
  pushq %rsi
  call nuah_host_jmpbuf@PLT
  popq %rsi
  movq %rax, %rdi
  jmp nuah_host_longjmp@PLT
  .size \name, .-\name
.endm

NUAH_LONGJMP longjmp
NUAH_LONGJMP _longjmp
NUAH_LONGJMP siglongjmp

.section .note.GNU-stack,"",@progbits
