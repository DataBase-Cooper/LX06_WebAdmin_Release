#include <security/pam_appl.h>
#include <security/pam_modules.h>
#include <security/pam_ext.h>
int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)flags; (void)argc; (void)argv;
    pam_set_item(pamh, PAM_TTY, (const void *)0);
    return PAM_SUCCESS;
}
int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)pamh; (void)flags; (void)argc; (void)argv;
    return PAM_SUCCESS;
}
