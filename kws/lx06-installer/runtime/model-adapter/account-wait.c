/* Provider-neutral resident mailbox wait only. No audio, account, lease,
 * connection or SDK initialization belongs in this standalone executable.
 * Reuse the reviewed implementation without changing its real error codes. */
#define _GNU_SOURCE
#include "experiments/account-wait.h"

int main(int argc, char **argv) {
    return aw_cli(argc, argv);
}
