; boot.S - minimal boot sector for the QEMU usb-storage CSW deadlock PoC.
;
; It loads the C payload from the boot disk (LBA 1..N) to linear 0x20000
; using BIOS extended disk reads, switches to 32-bit protected mode and
; calls cmain().
;
; The PoC image is used as a USB mass storage stick, SeaBIOS boots from
; it, so INT 13h is served by SeaBIOS' own USB stack.

BITS 16
ORG 0x7C00

%ifndef PAYLOAD_SECTORS
%define PAYLOAD_SECTORS 128
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl

    ; minimal real-mode serial init (COM1 115200 8N1) for debug markers
    mov dx, 0x3f9          ; IER
    xor al, al
    out dx, al
    mov dx, 0x3fb          ; LCR: DLAB on
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8          ; divisor low = 1
    mov al, 0x01
    out dx, al
    mov dx, 0x3f9          ; divisor high = 0
    xor al, al
    out dx, al
    mov dx, 0x3fb          ; LCR: 8N1, DLAB off
    mov al, 0x03
    out dx, al
    mov dx, 0x3f8
    mov al, 'B'            ; marker: boot sector started
    out dx, al

    ; enable A20 line (fast A20 gate, works on QEMU)
    in al, 0x92
    or al, 2
    out 0x92, al

    ; load payload: LBA 1..PAYLOAD_SECTORS -> linear 0x20000 (seg 0x2000, off 0)
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    ; switch to protected mode
    cli
    lgdt [gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

disk_error:
    mov dx, 0x3f8
    mov al, 'D'            ; marker: INT 13h failed
    out dx, al
.hang:
    hlt
    jmp .hang

; disk address packet for INT 13h AH=42h
dap:
    db 0x10
    db 0
    dw PAYLOAD_SECTORS
    dw 0            ; offset
    dw 0x2000       ; segment -> linear 0x20000
    dq 1            ; start LBA (sector 1, right after the boot sector)

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF  ; 0x08: code, base 0, limit 4G, 32-bit
    dq 0x00CF92000000FFFF  ; 0x10: data, base 0, limit 4G
gdtr:
    dw gdtr - gdt - 1
    dd gdt

boot_drive: dd 0

BITS 32
%ifndef CMAIN_ADDR
%define CMAIN_ADDR 0x20000
%endif

pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x7C00
    mov eax, CMAIN_ADDR
    push dword 0
    push dword [boot_drive]
    call eax
.halt:
    hlt
    jmp .halt

times 510-($-$$) db 0
dw 0xAA55
