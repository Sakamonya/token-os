// AArch64 异常向量表 + 中断入口
// 向量表必须 2KB 对齐，每个表项 128 字节，一共 16 项
    .section ".text", "ax"
    .global vector_table
    .global vector_table_end
    .balign 2048

vector_table:
    // ---- 当前异常级别，使用 SP_EL0 的栈 ----
    .balign 128
    b   sync_hang
    .balign 128
    b   irq_entry
    .balign 128
    b   fiq_hang
    .balign 128
    b   serr_hang

    // ---- 当前异常级别，使用 SP_ELx 的栈（我们跑在这里）----
    .balign 128
    b   sync_hang
    .balign 128
    b   irq_entry
    .balign 128
    b   fiq_hang
    .balign 128
    b   serr_hang

    // ---- 低异常级别，AArch64 ----
    .balign 128
    b   sync_hang
    .balign 128
    b   irq_entry
    .balign 128
    b   fiq_hang
    .balign 128
    b   serr_hang

    // ---- 低异常级别，AArch32 ----
    .balign 128
    b   sync_hang
    .balign 128
    b   irq_entry
    .balign 128
    b   fiq_hang
    .balign 128
    b   serr_hang

vector_table_end:

// 中断入口：保存 x0~x30，调 C 函数，恢复，eret
irq_entry:
    sub     sp, sp, #256
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    stp     x30, xzr, [sp, #16 * 15]

    bl      irq_handler

    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldp     x30, xzr, [sp, #16 * 15]
    add     sp, sp, #256

    eret

// 同步异常：打印现场后停机
sync_hang:
    mov     x0, #0              // 0 = 同步异常
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    b       fault_report

fiq_hang:
    mov     x0, #1              // 1 = FIQ
    b       fault_report

serr_hang:
    mov     x0, #2              // 2 = SError
    b       fault_report

fault_report:
    bl      fault_handler
9:
    wfe
    b       9b
