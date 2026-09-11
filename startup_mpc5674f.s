
	.section .startup, "ax"
        .globl	_start
_start:
		nop

;#****************************** Turn off SWT ********************************
	lis	r4, 0xFFF3
	ori	r4, r4, 0x8000

	lis	r3, 0xC520
	stw	r3, 0x10(r4)

	lis	r3, 0xD928
	stw	r3, 0x10(r4)

	lis	r3, 0xFF00
	ori r3,	r3, 0x010A
	stw	r3, 0(r4)

;#********************************* Enable BTB ********************************
;# Flush & Enable BTB - Set BBFI bit in BUCSR
	li	r3, 0x201
	mtspr	1013, r3
	isync

;#**************************** Init Core Registers ****************************
;# The E200Z4 core needs its registers initialising before they are used
;# otherwise in Lock Step mode the two cores will contain different random data.
;# If this is stored to memory (e.g. stacked) it will cause a Lock Step error.

;# GPRs 0-31
	li	r0, 0
	li	r1, 0
	li	r2, 0
	li	r3, 0
	li	r4, 0
	li	r5, 0
	li	r6, 0
	li	r7, 0
	li	r8, 0
	li	r9, 0
	li	r10, 0
	li	r11, 0
	li	r12, 0
	li	r13, 0
	li	r14, 0
	li	r15, 0
	li	r16, 0
	li	r17, 0
	li	r18, 0
	li	r19, 0
	li	r20, 0
	li	r21, 0
	li	r22, 0
	li	r23, 0
	li	r24, 0
	li	r25, 0
	li	r26, 0
	li	r27, 0
	li	r28, 0
	li	r29, 0
	li	r30, 0
	li	r31, 0

;# Init any other CPU register which might be stacked (before being used).

	mtspr	1, r1		;#XER
	mtcrf   0xFF, r1
	mtspr   CTR, r1
	mtspr	272, r1		;#SPRG0
	mtspr	273, r1		;#SPRG1
	mtspr	274, r1		;#SPRG2
	mtspr	275, r1		;#SPRG3
	mtspr	58, r1		;#CSRR0
	mtspr	59, r1		;#CSRR1
	mtspr	570, r1		;#MCSRR0
	mtspr	571, r1		;#MCSRR1
	mtspr	61, r1		;#DEAR
	mtspr	63, r1		;#IVPR
	mtspr	256, r1		;#USPRG0
	mtspr	62, r1		;#ESR
	mtspr	8, r31		;#LR

;#*************************** Enable ME Bit in MSR *****************************
	mfmsr	r6
	ori      r6, r6,0x1000
	mtmsr	r6

#if defined(SPE_ENABLE)
;#*************************** Enable SPE Bit in MSR *****************************
		mfmsr r3
		ori      r3, r3,0x0200
		mtmsr r3
#endif

;#****************************** Initialize BSS section ******************************/
bss_Init:
    lis        r9, __BSS_SIZE@h       # Load upper BSS load size (# of bytes) into R9
    ori      r9, r9, __BSS_SIZE@l       # Load lower BSS load size into R9 and compare to zero
    cmpwi     r9,0
    beq        bss_Init_end           # Exit if size is zero (no data to initialise)

    mtctr        r9                     # Store no. of bytes to be moved in counter

    lis        r5, __BSS_START@h      # Load upper BSS address into R5 (from linker file)
    ori      r5, r5, __BSS_START@l      # Load lower BSS address into R5 (from linker file)
    subi       r5, r5, 1              # Decrement address to prepare for bss_Init_loop

    lis        r4, 0x0

bss_Init_loop:
    stbu       r4, 1(r5)              # Store zero byte into BSS at R5 and update BSS address
    bdnz       bss_Init_loop          # Branch if more bytes to load

bss_Init_end:

;#****************************** Configure Stack ******************************/
	lis	r1, __SP_INIT@h	;# Initialize stack pointer r1 to
	ori	r1, r1, __SP_INIT@l	;# value in linker command file.

	lis	r13, _SDA_BASE_@h	;# Initialize r13 to sdata base
	ori	r13, r13,  _SDA_BASE_@l	;# (provided by linker).

	lis	r2, _SDA2_BASE_@h	;# Initialize r2 to sdata2 base
	ori	r2, r2, _SDA2_BASE_@l	;# (provided by linker).

	stwu	r0,-64(r1)			;# Terminate stack.

;#****************************** Run ctors ******************************/
	bl	RunGlobalConstructors

;# Jump to Main
	bl	main
KernelReturned:
	b	KernelReturned

RunGlobalConstructors:
	stwu	r1, -16(r1)
	mflr	r0
	stw	r0, 20(r1)

	lis	r3, __preinit_array_start@h
	ori	r3, r3, __preinit_array_start@l
	lis	r4, __preinit_array_end@h
	ori	r4, r4, __preinit_array_end@l
	bl	CallFunctionArray

	;# Supplied by the PowerPC runtime. Among other runtime initialization,
	;# this invokes the legacy .ctors list in its required reverse order.
	bl	__init

	lis	r3, __init_array_start@h
	ori	r3, r3, __init_array_start@l
	lis	r4, __init_array_end@h
	ori	r4, r4, __init_array_end@l
	bl	CallFunctionArray

	lwz	r0, 20(r1)
	mtlr	r0
	addi	r1, r1, 16
	blr

;# Call each non-null function pointer in the half-open range [r3, r4).
CallFunctionArray:
	stwu	r1, -24(r1)
	mflr	r0
	stw	r0, 28(r1)
	stw	r30, 16(r1)
	stw	r31, 20(r1)
	mr	r30, r3
	mr	r31, r4
CallFunctionArrayLoop:
	cmplw	r30, r31
	beq	CallFunctionArrayComplete
	lwz	r12, 0(r30)
	addi	r30, r30, 4
	cmpwi	r12, 0
	beq	CallFunctionArrayLoop
	mtctr	r12
	bctrl
	b	CallFunctionArrayLoop
CallFunctionArrayComplete:
	lwz	r30, 16(r1)
	lwz	r31, 20(r1)
	lwz	r0, 28(r1)
	mtlr	r0
	addi	r1, r1, 24
	blr

;# Enter the flash-resident E92 bootloader upload routine through its exported
;# application API.  The first function pointer in the bootloader API table at
;# 0x0000FF80 points to the upload entry (0x0000B1F0 in the analyzed firmware).
	.section .text_booke, "ax"
	.align 2
	.globl ExitToBootloaderUploadRoutine
	.type ExitToBootloaderUploadRoutine, @function
ExitToBootloaderUploadRoutine:
	wrteei	0
;# The upload entry clears ordinary SRAM through 0x4001BFFF, including the
;# kernel stack.  Use the bootloader's locked cache-as-RAM stack instead.
	lis	r1, 0x6000
	ori	r1, r1, 0x3FF0
	lis	r12, 0x0001
	lwz	r12, -0x0080(r12)	;# bootloader API[0] at 0x0000FF80
	mtctr	r12
	bctr
	.size ExitToBootloaderUploadRoutine, .-ExitToBootloaderUploadRoutine
