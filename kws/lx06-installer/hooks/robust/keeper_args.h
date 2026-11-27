/* 管理端 args 兼容空格/引号，但绝不求值变量、重定向、管道或命令替换。
 * 不把参数塞进 sh -c：即使出现 ;、$()，也只是被执行程序收到的普通参数。
 */
#ifndef LX06_KEEPER_ARGS_H
#define LX06_KEEPER_ARGS_H
#include <ctype.h>
#include "keeper_client.h"
static inline int lk_split_args(const char *text, struct lk_request *req) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        size_t n = 0;
        unsigned char quote = 0;
        char *out;
        while (*p && isspace(*p)) p++;
        if (!*p) break;
        if (req->argc >= LK_ARGC) { errno = E2BIG; return -1; }
        out = req->argv[req->argc];
        while (*p && (quote || !isspace(*p))) {
            unsigned char c = *p++;
            if (c == '\\' && quote != '\'') {
                if (!*p) { errno = EINVAL; return -1; }
                c = *p++;
            } else if (c == '\'' || c == '"') {
                if (!quote) { quote = c; continue; }
                if (quote == c) { quote = 0; continue; }
            }
            if (n + 1 >= LK_ARG_LEN) { errno = E2BIG; return -1; }
            out[n++] = (char)c;
        }
        if (quote) { errno = EINVAL; return -1; }
        out[n] = 0;
        req->argc++;
    }
    return 0;
}
#endif
