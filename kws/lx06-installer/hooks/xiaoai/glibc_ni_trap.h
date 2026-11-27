/* 静态 glibc ≥2.36 经 __libc_do_syscall 发 64 位 time 系调用。
 * mico 4.9.61 没有 403/422/436/397/398/435，每次都打 do_ni_syscall。
 * 同名遮蔽后对这些号直接返回 -ENOSYS，glibc 走旧号回退。
 * 与 xiaoai-mic.c 里的拦截是同一段，各二进制单独编一份。 */
#ifndef GLIBC_NI_TRAP_H
#define GLIBC_NI_TRAP_H
#include <sys/syscall.h>
#if defined(__arm__)
__attribute__((naked, used))
long __libc_do_syscall(long n, long a0, long a1, long a2,
                       long a3, long a4, long a5) {
    (void)n; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    __asm__ __volatile__ (
        "push {r7, fp, ip, lr}\n"
        "mov fp, r12\n"
        "movw ip, #397\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #398\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #403\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #422\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #435\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #436\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "mov r7, fp\n"
        "svc #0\n"
        "pop {r7, fp, ip, pc}\n"
        "1:\n"
        "mvn r0, #37\n"
        "pop {r7, fp, ip, pc}\n"
        : : : "memory");
}
#endif
#endif
