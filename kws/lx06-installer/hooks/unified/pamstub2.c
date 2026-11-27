/* svr_auth_pam 桩：集成版不用 PAM（直接 crypt 认证），仅满足链接 */
int svr_auth_pam(int type, const char *user, const char *passwd, int *authfail, int *pwfail){
  (void)type;(void)user;(void)passwd;(void)authfail;(void)pwfail; return 1;
}
