#ifndef XZ_TALK_H
#define XZ_TALK_H

#include <stdio.h>

/* 对话日志（xiaozhi.talk.log）：记录"用户说了什么 / 小智答了什么"。
   与旧版单文件客户端的 TALK_LOG 格式一致：[HH:MM:SS] role: text。
   硬上限 XZ_TALK_LIMIT_BYTES：超过后自动截断、只保留最近
   XZ_TALK_KEEP_BYTES（不会写满系统空间；keeper 受限作业的
   RLIMIT_FSIZE=64KB 之外再加这道保险，root/无限制运行也生效）。 */

typedef struct xz_talk {
    FILE *stream;
} xz_talk;

/* path 为 NULL/空/"0"/"none" 时返回 NULL（禁用）。打开失败也返回 NULL（不致命）。 */
xz_talk *xz_talk_open(const char *path);
void xz_talk_close(xz_talk *talk);

/* 追加一行对话记录；text 内的换行替换为空格，保证一行一条。 */
void xz_talk_log(xz_talk *talk, const char *role, const char *text);

/* SIGXFSZ 处理用：暴露底层 FILE*（async-signal-safe 的 ftruncate）。 */
FILE *xz_talk_stream(const xz_talk *talk);

#endif
