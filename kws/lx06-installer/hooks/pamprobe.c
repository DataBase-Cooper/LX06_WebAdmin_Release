/* lx06 pam probe: run real PAM (sshd service) on-device, feed a password,
 * report every return code. Built with the project cross-toolchain. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <shadow.h>

/* glibc 2.25 crypt/crypt.h 真实布局（131KB，DES 表 4×32768）：
 * keysched[128] + sb0..sb3[32768] + crypt_3_buf[14] + current_salt[2]
 * + long current_saltbits + int direction + int initialized。
 * 之前用 {int;char[256]} 小布局 → crypt_r 写越界 → qemu 段错误。
 * 设备未装 crypt.h，按源码树 glibc-2.25/crypt/crypt.h 手写。 */
struct crypt_data {
  char keysched[16 * 8];
  char sb0[32768];
  char sb1[32768];
  char sb2[32768];
  char sb3[32768];
  char crypt_3_buf[14];
  char current_salt[2];
  long int current_saltbits;
  int direction, initialized;
};
char *crypt(const char *, const char *);
char *crypt_r(const char *, const char *, struct crypt_data *);

typedef struct { int msg_style; const char *msg; } pm_t;
typedef struct { char *resp; int resp_retcode; } pr_t;
typedef int (*cvf)(int, const pm_t **, pr_t **, void *);
struct pconv { cvf c; void *d; };
typedef struct phandle phandle_t;
#define PAM_PROMPT_ECHO_OFF 2
#define PAM_SUCCESS 0
extern int pam_start(const char *, const char *, const struct pconv *, phandle_t **);
extern int pam_authenticate(phandle_t *, int);
extern int pam_acct_mgmt(phandle_t *, int);
extern int pam_open_session(phandle_t *, int);
extern const char *pam_strerror(phandle_t *, int);
extern void pam_end(phandle_t *, int);

static int conv(int n, const pm_t **m, pr_t **r, void *d) {
  *r = (pr_t *)calloc(n ? n : 1, sizeof(**r));
  for (int i = 0; i < n; i++) {
    (*r)[i].resp_retcode = 0;
    if (m[i]->msg_style == PAM_PROMPT_ECHO_OFF) {
      (*r)[i].resp = (char *)d;   /* password passed via appdata */
    }
  }
  return PAM_SUCCESS;
}

int main(int argc, char **argv) {
  const char *svc = argc > 1 ? argv[1] : "sshd";
  const char *pw = argc > 2 ? argv[2] : "";
  struct pconv c = {conv, (void *)pw};
  phandle_t *p = 0;
  int s = pam_start(svc, "root", &c, &p);
  printf("start=%d (%s)\n", s, s == 0 ? "ok" : pam_strerror(0, s));
  if (s) return 1;
  /* NSS 直测: getpwnam / getspnam_r 能否读到 root (pam_unix 认证的前提) */
  struct passwd *epw = getpwnam("root");
  printf("getpwnam(root)=%s uid=%d\n", epw ? "OK" : "NULL", epw ? epw->pw_uid : -1);
  struct spwd *sp = getspnam("root");
  if (sp) printf("getspnam(root) OK sp_pwdp=%s\n", sp->sp_pwdp && *sp->sp_pwdp ? sp->sp_pwdp : "(empty)");
  else printf("getspnam(root)=NULL\n");
  /* crypt_r 直测：linux-pam verify_pwd_hash 里 21 的主嫌疑是 crypt_r 返回 NULL。
   * 用 shadow 里的 hash 直接调 crypt/crypt_r，独立于 PAM 栈给出结论。 */
  if (sp && sp->sp_pwdp && *sp->sp_pwdp) {
    struct crypt_data d;
    char *c1 = crypt(pw, sp->sp_pwdp);
    char *c2 = crypt_r(pw, sp->sp_pwdp, &d);
    printf("crypt  = %s\n", c1 ? c1 : "(NULL)");
    printf("crypt_r= %s\n", c2 ? c2 : "(NULL)");
    printf("match   = %s\n",
           (c1 && c2 && strcmp(c1, sp->sp_pwdp) == 0 && strcmp(c2, sp->sp_pwdp) == 0) ? "YES" : "NO");
  }
  s = pam_authenticate(p, 0);
  printf("auth=%d (%s)\n", s, pam_strerror(0, s));
  s = pam_acct_mgmt(p, 0);
  printf("acct=%d (%s)\n", s, pam_strerror(0, s));
  s = pam_open_session(p, 0);
  printf("sess=%d (%s)\n", s, pam_strerror(0, s));
  pam_end(p, 0);
  return 0;
}
