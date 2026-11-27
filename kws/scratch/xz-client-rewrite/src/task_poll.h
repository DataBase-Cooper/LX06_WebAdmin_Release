#ifndef XZ_TASK_POLL_H
#define XZ_TASK_POLL_H

#include <stddef.h>
#include <stdint.h>

#define XZ_TASK_ROOT "/tmp/lx06-xz-mcp-task"
#define XZ_TASK_PATH_MAX 512
#define XZ_TASK_ID_MAX 48
#define XZ_TASK_QUERY_MAX 160
#define XZ_TASK_MAX_PENDING 8
#define XZ_TASK_POLL_MS 35000u
#define XZ_TASK_REPLY_MS 8000u

typedef enum { XZ_TASK_NONE, XZ_TASK_RUNNING, XZ_TASK_READY, XZ_TASK_QUERIED } xz_task_phase;

typedef struct {
    char id[XZ_TASK_ID_MAX + 1];
    char query[XZ_TASK_QUERY_MAX + 1];
    xz_task_phase phase;
    uint64_t query_at;
} xz_task;

typedef struct {
    xz_task tasks[XZ_TASK_MAX_PENDING];
    size_t count;
    uint64_t refresh_at;
    uint64_t awaiting_until;
} xz_task_poll;

void xz_task_poll_refresh(xz_task_poll *poll, const char *path, uint64_t now);
int xz_task_session_open(const char *root, char path[XZ_TASK_PATH_MAX]);
void xz_task_session_close(char path[XZ_TASK_PATH_MAX]);
xz_task *xz_task_poll_next(xz_task_poll *poll, const char *fallback, uint64_t now, int *active);
int xz_task_query_text(char *out, size_t cap, const char *pattern, const char *id);

#endif
