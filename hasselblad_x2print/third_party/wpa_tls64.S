/* wpa_tls64.S — PT_TLS alignment shim for bionic arm64 static binaries
 *
 * Background: the bionic (arm64) dynamic linker/static startup code requires the executable
 * PT_TLS segment to have alignment >=64 (TCB/SVE requirement), or the program aborts at startup:
 *   "executable's TLS segment is underaligned: alignment is 8, needs to be
 *    at least 64 for ARM64 Bionic"
 * lld once relaxed ARM/AArch64 PT_TLS alignment in D53906, then reverted it in D62055.
 * Therefore, neither NDK r25's lld-14 nor upstream lld-18 relaxes it automatically. Static
 * bionic libc.a includes .tdata/.tbss aligned to 8, which triggers the failure.
 * Workaround: link a dummy .tdata section aligned to 64 bytes; lld uses the maximum
 * alignment of input TLS sections, producing PT_TLS p_align=64 and 64-byte-aligned vaddr
 * (verified with qemu-aarch64; see NOTES §14).
 */
    .section .tdata,"awT",@progbits
    .balign 64
    .globl __wpa_tls_align64_dummy
    .type __wpa_tls_align64_dummy, %object
__wpa_tls_align64_dummy:
    .quad 0
    .size __wpa_tls_align64_dummy, 8

    .text
    .globl __wpa_tls64_marker
    .type __wpa_tls64_marker, %function
__wpa_tls64_marker:
    ret
