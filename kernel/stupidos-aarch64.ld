ENTRY(_start)

SECTIONS
{
    /*
     * QEMU `-kernel` 在 virt 机型下默认把 Image 放到 0x40080000。
     * 这里必须与真实装载地址一致，否则函数指针/异常向量等“绝对地址常量”
     * 会和运行时实际地址错开 0x80000，最终表现为随机跳转和指令异常。
     */
    . = 0x40080000;
    __kernel_start = .;
    __text_start = .;
    .text :
    {
        KEEP(*(.text.boot))
        KEEP(*(.vectors))
        *(.text)
        *(.text.*)
    }
    . = ALIGN(4K);
    __text_end = .;

    __data_start = .;
    .data :
    {
        *(.data)
        *(.data.*)
    }
    . = ALIGN(4K);

    idmap_pgd = .;
    . += 4096;
    . = ALIGN(4K);

    idmap_pud = .;
    . += 2 * 4096;
    . = ALIGN(4K);

    idmap_pmd = .;
    . += 4 * 4096;
    . = ALIGN(4K);

    idmap_pt = .;
    . += 16 * 4096;
    . = ALIGN(4K);

    swapper_pgd = .;
    . += 4096;
    . = ALIGN(4K);

    swapper_pud = .;
    . += 4 * 4096;
    . = ALIGN(4K);

    swapper_pmd = .;
    . += 8 * 4096;
    . = ALIGN(4K);

    swapper_pt = .;
    . += 32 * 4096;
    . = ALIGN(4K);

    init_stack_start = .;
    . += 4096;
    . = ALIGN(4K);
    init_stack_end = .;

    . = ALIGN(4K);
    __data_end = .;

    __rodata_start = .;
    .rodata :
    {
        *(.rodata)
        *(.rodata.*)
    }
    . = ALIGN(4K);
    __rodata_end = .;

    __bss_start = .;
    .bss :
    {
        *(.bss)
        *(.bss.*)
    }
    . = ALIGN(4K);
    __bss_end = .;

    __kernel_end = .;
}
