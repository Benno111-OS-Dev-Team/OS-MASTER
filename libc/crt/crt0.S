/*
 * UnixOS - C Runtime Startup Code (crt0)
 * 
 * Entry point for userspace programs.
 */

.global _start
.type _start, %function

.extern main
.extern exit
.extern environ

_start:
    /* Clear frame pointer for stack traces */
    mov     x29, #0
    mov     x30, #0
    
    /* argc is in x0, argv in x1, envp in x2 (from kernel) */
    /* Publish envp for getenv() and friends before calling main. */
    adrp    x9, environ
    add     x9, x9, :lo12:environ
    str     x2, [x9]
    
    /* Call main(argc, argv, envp) */
    bl      main
    
    /* Exit with return value from main */
    bl      exit
    
    /* Should not reach here, but just in case */
1:
    wfi
    b       1b

.size _start, . - _start
