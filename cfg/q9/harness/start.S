.global _start
_start:
	call main
	movl %eax, %ebx
	movl $252, %eax
	int $0x80
	hlt
