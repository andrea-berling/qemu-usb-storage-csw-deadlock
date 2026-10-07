OUTPUT_FORMAT(elf32-i386)
ENTRY(cmain)

SECTIONS
{
    . = 0x20000;

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    /* 4K alignment for the qTD buffer pages */
    .data : ALIGN(4096) {
        *(.data .data.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
