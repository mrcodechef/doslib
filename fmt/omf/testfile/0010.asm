
	PROG segment para public 'CODE'
	assume cs:PROG
	assume ds:PROG

	org	100h

	nop
_start:	mov	ax,4C00h
	int	21h
	ret

	PROG ends

	end _start

