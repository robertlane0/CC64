/* Target entry contract: RDI=PSP, loader-owned return address on stack. */
.text
.global cc64_start
.type cc64_start,@function
cc64_start:
    xor %edi, %edi
    xor %esi, %esi
    call main
    mov %ecx, %eax
    mov $0x4c00, %eax
    mov %cl, %al
    int $0x21
    ret
.size cc64_start, .-cc64_start
