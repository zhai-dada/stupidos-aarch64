ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    /*
     * 把 userspace ELF 的链接基址抬到非零页，避免 e_entry=0：
     * - exec_task_main 会把空入口当成无效镜像直接退出
     * - 低地址 alias 运行时也尽量避免把 NULL 页当可执行入口
     */
    . = 0x10000;

    .text :
    {
        *(.text.start)
        *(.text)
        *(.text.*)
    } :text

    .rodata :
    {
        *(.rodata)
        *(.rodata.*)
    } :text

    . = ALIGN(0x1000);

    .data :
    {
        *(.data)
        *(.data.*)
    } :data

    .bss :
    {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    } :data
}
