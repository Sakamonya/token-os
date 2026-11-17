// AArch64 启动汇编：设栈 -> 清 BSS -> 跳 main
// 兼容三种入口异常级别（QEMU virt 通常直接落在 EL1，但上真板子可能是 EL2/EL3）
    .section ".text.boot", "ax"
    .global _start

_start:
    // 读 CurrentEL，判断当前在哪个异常级别
    mrs     x0, CurrentEL
    and     x0, x0, #0xc
    cmp     x0, #0x8            // 0x8 = EL2
    b.eq    from_el2
    cmp     x0, #0xc            // 0xc = EL3
    b.eq    from_el3
    b       el1_entry           // 已经在 EL1，直接进

from_el3:
    // SCR_EL3: RW=1 (EL1 是 AArch64), NS=1 (非安全世界)
    mov     x0, #0x5b1
    msr     scr_el3, x0
    // SPSR_EL3: 屏蔽 DAIF，返回后进 EL1h
    mov     x0, #0x3c5
    msr     spsr_el3, x0
    adr     x0, el1_entry
    msr     elr_el3, x0
    eret

from_el2:
    // HCR_EL2.RW = 1 -> EL1 是 AArch64
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    // SPSR_EL2: 屏蔽 DAIF，返回后进 EL1h
    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

el1_entry:
    // 关掉 MMU / 数据缓存（QEMU 默认就没开，显式保证一下）
    mrs     x1, sctlr_el1
    bic     x1, x1, #(1 << 0)    // M  = 0 关 MMU
    bic     x1, x1, #(1 << 2)    // C  = 0 关数据缓存
    msr     sctlr_el1, x1
    isb

    // 栈
    ldr     x1, =__stack_top
    mov     sp, x1

    // 清 BSS
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:
    cmp     x1, x2
    b.ge    2f
    str     xzr, [x1], #8
    b       1b
2:

    bl      main

    // main 不该返回；万一返回了就死循环
3:
    wfe
    b       3b
