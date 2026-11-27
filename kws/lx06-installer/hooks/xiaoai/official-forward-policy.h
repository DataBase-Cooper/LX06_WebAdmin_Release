/* Experimental pure decision seam. NOT wired to native wake or web UI.
 * Caller must establish fresh trusted registry state and revalidate at dispatch.
 * This function does not read/persist settings, preempt or launch anything. */
#ifndef LX06_OFFICIAL_FORWARD_POLICY_H
#define LX06_OFFICIAL_FORWARD_POLICY_H
#include <stddef.h>
#include <string.h>
enum of_result { OF_REJECT, OF_OFF, OF_OFFICIAL, OF_SCRIPT };
static enum of_result of_decide(int official_switch, int scripts_enabled,
                                const char *selection, int exists, int eligible) {
    if ((official_switch != 0 && official_switch != 1) ||
        (scripts_enabled != 0 && scripts_enabled != 1)) return OF_REJECT;
    if (!official_switch) return OF_OFF;
    if (!selection) return OF_REJECT;
    if (!strcmp(selection,"official")) return OF_OFFICIAL;
    size_t n=strlen(selection);
    if (!n || n>64 || selection[0]=='.') return OF_REJECT;
    for (size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)selection[i];
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
              (c>='0'&&c<='9')||c=='.'||c=='_'||c=='-')) return OF_REJECT;
    }
    if (!scripts_enabled || exists!=1 || eligible!=1) return OF_REJECT;
    return OF_SCRIPT;
}
#endif
