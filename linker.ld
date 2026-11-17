/* QEMU virt (cortex-a72) 的最小裸机链接脚本
 * 加载地址 0x40000000 是 qemu-system-aarch64 -M virt -kernel 的默认入口
 */
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;
    __kernel_start = .;

    /* 启动代码必须放在最前面：上电后第一条指令就在这 */
    .text.boot : {
        *(.text.boot)
    }

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    __bss_end = .;

    /* 64 KiB 栈，先保证够用 */
    . = ALIGN(16);
    . = . + 0x10000;
    __stack_top = .;

    __kernel_end = .;
}
