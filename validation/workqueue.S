.text
.globl run_reuse
run_reuse:
    movq %rsp, saved_rsp(%rip)
    movq %rbp, saved_rbp(%rip)
    movq %rbx, saved_rbx(%rip)
    movq %r12, saved_r12(%rip)
    movq %r13, saved_r13(%rip)
    movq %r14, saved_r14(%rip)
    movq %r15, saved_r15(%rip)
    jmp wqueue_entry_point_asm_jump
.globl reuse_entry
reuse_entry:
    movq %rsp, observed_rsp(%rip)
    movq %rbp, observed_rbp(%rip)
    cmpl $123, %esi
    jne .failed
    cmpq $456, %rdx
    jne .failed
    cmpq $789, %rcx
    jne .failed
    cmpl $321, %r9d
    jne .failed
    incl reuse_count(%rip)
    cmpl $10000, reuse_count(%rip)
    je .done
    subq $256, %rsp
    call wqueue_entry_point_asm_jump
    ud2
.failed:
    ud2
.done:
    movq saved_rsp(%rip), %rsp
    movq saved_rbp(%rip), %rbp
    movq saved_rbx(%rip), %rbx
    movq saved_r12(%rip), %r12
    movq saved_r13(%rip), %r13
    movq saved_r14(%rip), %r14
    movq saved_r15(%rip), %r15
    ret
.section .note.GNU-stack,"",@progbits
