#ifndef XZ_HOME_MCP_H
#define XZ_HOME_MCP_H

#include "ws.h"
#include <sys/types.h>

typedef struct {
    const char *provider;
    pid_t pid;
    int fd;
    uint64_t deadline;
    uint64_t next_call;
    size_t used;
    char reply[1024];
    char id[96];
    char device[97], action[24], value[8];
    uint64_t requested_at;
    int from_voice, accepted;
} xz_home_mcp;

void xz_home_mcp_configure(xz_home_mcp *state, const char *provider);
int xz_home_mcp_request(xz_home_mcp *state, xz_ws *ws, const char *session,
                     const char *message, size_t length, uint64_t now, int closing);
int xz_home_mcp_voice(xz_home_mcp *state, xz_ws *ws, const char *text, uint64_t now);
int xz_home_mcp_poll(xz_home_mcp *state, xz_ws *ws, const char *session, uint64_t now);
void xz_home_mcp_close(xz_home_mcp *state);

#endif
