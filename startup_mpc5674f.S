
	.section .startup, "ax"
        .globl	_start
_start:
		nop

#****************************** Turn off SWT ********************************
	lis	4, 0xFFF3
	ori	4, 4, 0x8000

	lis	3, 0xC520
	stw	3, 0x10(4)

	lis	3, 0xD928
	stw	3, 0x10(4)

	lis	3, 0xFF00
	ori 3,	3, 0x010A
	stw	3, 0(4)

#********************************* Enable BTB ********************************
# Flush & Enable BTB - Set BBFI bit in BUCSR
	li	3, 0x201
	mtspr	1013, 3
	isync

#**************************** Init Core Registers ****************************
# Start with deterministic GPR values before setting up the runtime.

# GPRs 0-31
	li	0, 0
	li	1, 0
	li	2, 0
	li	3, 0
	li	4, 0
	li	5, 0
	li	6, 0
	li	7, 0
	li	8, 0
	li	9, 0
	li	10, 0
	li	11, 0
	li	12, 0
	li	13, 0
	li	14, 0
	li	15, 0
	li	16, 0
	li	17, 0
	li	18, 0
	li	19, 0
	li	20, 0
	li	21, 0
	li	22, 0
	li	23, 0
	li	24, 0
	li	25, 0
	li	26, 0
	li	27, 0
	li	28, 0
	li	29, 0
	li	30, 0
	li	31, 0

# Init any other CPU register which might be stacked (before being used).

	mtspr	1, 1		#XER
	mtcrf   0xFF, 1
	mtspr   9, 1
	mtspr	272, 1		#SPRG0
	mtspr	273, 1		#SPRG1
	mtspr	274, 1		#SPRG2
	mtspr	275, 1		#SPRG3
	mtspr	58, 1		#CSRR0
	mtspr	59, 1		#CSRR1
	mtspr	570, 1		#MCSRR0
	mtspr	571, 1		#MCSRR1
	mtspr	61, 1		#DEAR
	mtspr	63, 1		#IVPR
	mtspr	256, 1		#USPRG0
	mtspr	62, 1		#ESR
	mtspr	8, 31		#LR

#*************************** Enable ME Bit in MSR *****************************
	mfmsr	6
	ori      6, 6,0x1000
	mtmsr	6

# Enable SPE (MSR mask 0x02000000) before any compiled code executes.
# Initialize the SPE floating-point status/control register.
    mfmsr 3
    oris 3, 3, 0x0200
    mtmsr 3
    isync
    mtspr 512, 0

#****************************** Initialize BSS section ******************************/
bss_Init:
    lis        9, __BSS_SIZE@h       # Load upper BSS load size (# of bytes) into R9
    ori      9, 9, __BSS_SIZE@l       # Load lower BSS load size into R9 and compare to zero
    cmpwi     9,0
    beq        bss_Init_end           # Exit if size is zero (no data to initialise)

    mtctr        9                     # Store no. of bytes to be moved in counter

    lis        5, __BSS_START@h      # Load upper BSS address into R5 (from linker file)
    ori      5, 5, __BSS_START@l      # Load lower BSS address into R5 (from linker file)
    subi       5, 5, 1              # Decrement address to prepare for bss_Init_loop

    lis        4, 0x0

bss_Init_loop:
    stbu       4, 1(5)              # Store zero byte into BSS at R5 and update BSS address
    bdnz       bss_Init_loop          # Branch if more bytes to load

bss_Init_end:

#****************************** Configure Stack ******************************/
	lis	1, __SP_INIT@h	# Initialize stack pointer 1 to
	ori	1, 1, __SP_INIT@l	# value in linker command file.

	stwu	0,-64(1)			# Terminate stack.

#****************************** Run ctors ******************************/
	bl	RunGlobalConstructors

# Jump to Main
	bl	main
KernelReturned:
	b	KernelReturned

RunGlobalConstructors:
	stwu	1, -16(1)
	mflr	0
	stw	0, 20(1)

	lis	3, __preinit_array_start@h
	ori	3, 3, __preinit_array_start@l
	lis	4, __preinit_array_end@h
	ori	4, 4, __preinit_array_end@l
	bl	CallFunctionArray

	lis	3, __init_array_start@h
	ori	3, 3, __init_array_start@l
	lis	4, __init_array_end@h
	ori	4, 4, __init_array_end@l
	bl	CallFunctionArray

	lwz	0, 20(1)
	mtlr	0
	addi	1, 1, 16
	blr

# Call each non-null function pointer in the half-open range [3, 4).
CallFunctionArray:
	stwu	1, -32(1)
	mflr	0
	stw	0, 36(1)
	stw	30, 16(1)
	stw	31, 20(1)
	mr	30, 3
	mr	31, 4
CallFunctionArrayLoop:
	cmplw	30, 31
	beq	CallFunctionArrayComplete
	lwz	12, 0(30)
	addi	30, 30, 4
	cmpwi	12, 0
	beq	CallFunctionArrayLoop
	mtctr	12
	bctrl
	b	CallFunctionArrayLoop
CallFunctionArrayComplete:
	lwz	30, 16(1)
	lwz	31, 20(1)
	lwz	0, 36(1)
	mtlr	0
	addi	1, 1, 32
	blr

# Enter the flash-resident E92 bootloader upload routine through its exported
# application API.  The first function pointer in the bootloader API table at
# 0x0000FF80 points to the upload entry (0x0000B1F0 in the analyzed firmware).
	.section .text_booke, "ax"
	.align 2
	.globl ExitToBootloaderUploadRoutine
	.type ExitToBootloaderUploadRoutine, @function
ExitToBootloaderUploadRoutine:
	wrteei	0
# The upload entry clears ordinary SRAM through 0x4001BFFF, including the
# kernel stack.  Use the bootloader's locked cache-as-RAM stack instead.
	lis	1, 0x6000
	ori	1, 1, 0x3FF0
	lis	12, 0x0001
	lwz	12, -0x0080(12)	# bootloader API[0] at 0x0000FF80
	mtctr	12
	bctr
	.size ExitToBootloaderUploadRoutine, .-ExitToBootloaderUploadRoutine

