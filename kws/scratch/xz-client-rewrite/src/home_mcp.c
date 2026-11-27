#include "home_mcp.h"
#include "json.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef XZ_HOME_MCP_APPS
#define XZ_HOME_MCP_APPS "/data/lx06/apps"
#endif

#define HOME_TOOL "self.smart_home.control"
#define HOME_DUPLICATE_MS 5000
static const char list[] =
    "{\"tools\":[{\"name\":\"" HOME_TOOL "\","
    "\"description\":\"控制智能家居。用户明确提出设备操作时调用。"
    "device 填设备名，可带房间，例如客厅灯。受理不代表设备已执行。\","
    "\"inputSchema\":{\"type\":\"object\",\"required\":[\"device\",\"action\"],"
    "\"additionalProperties\":false,\"properties\":{"
    "\"device\":{\"type\":\"string\",\"description\":\"设备名或房间加设备名\"},"
    "\"action\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"brightness\",\"temperature\"]},"
    "\"value\":{\"type\":\"integer\",\"description\":\"亮度百分比或空调摄氏度，仅调节时填写\"}}}}]}";

static int send_result(xz_ws *ws, const char *session, const char *id,
                       const char *kind, const char *body) {
    char payload[1800], envelope[2048];
    int n = snprintf(payload, sizeof(payload), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"%s\":%s}", id, kind, body);
    if (n < 0 || (size_t)n >= sizeof(payload) ||
        xz_protocol_wrap_mcp(envelope, sizeof(envelope), session, payload)) return -1;
    return xz_ws_send_text(ws, envelope);
}

static int rpc_id(const xz_json_doc *doc, int payload, char *out, size_t capacity) {
    int index = xz_json_object_get(doc, payload, "id");
    if (index < 0) return -1; /* Notifications receive no reply. */
    const xz_json_token *id = &doc->tokens[index];
    size_t start = id->start, end = id->end;
    if (id->type == XZ_JSON_STRING) {
        --start;
        ++end;
    } else {
        int number;
        if (xz_json_get_int(doc, payload, "id", &number)) return -1;
        int size = snprintf(out, capacity, "%d", number);
        return size >= 0 && (size_t)size < capacity ? 0 : -1;
    }
    if (end - start >= capacity) return -1;
    memcpy(out, doc->source + start, end - start);
    out[end - start] = '\0';
    return 0;
}

/* Treat this argument as a short device name, never a full voice command.
 * Accept ASCII names and common CJK characters used by Mi Home. */
static int device_name(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    if (!*p || *p == ' ') return 0;
    while (*p) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= 'a' && *p <= 'z') || *p == '-' || *p == '_' || (*p == ' ' && p[1])) {
            ++p;
            continue;
        }
        if (*p < 0xe3 || *p > 0xef || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) return 0;
        unsigned cp = ((unsigned)(p[0] & 15) << 12) | ((unsigned)(p[1] & 63) << 6) | (p[2] & 63);
        if (!((cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff))) return 0;
        p += 3;
    }
    static const char *const clauses[] = {"然后", "同时", "并且", "以及", "顺便", "或者",
                                          "打开", "关闭", "再把", "再开", "再关"};
    for (size_t i = 0; i < sizeof(clauses) / sizeof(clauses[0]); ++i)
        if (strstr(text, clauses[i])) return 0;
    return 1;
}

static int decode_action(const xz_json_doc *doc, int arguments, char device[97],
                         char action[24], char value_text[4]) {
    if (xz_json_get_string(doc, arguments, "device", device, 97) ||
        xz_json_get_string(doc, arguments, "action", action, 24) ||
        !device_name(device)) return -1;
    value_text[0] = '\0';
    if (!strcmp(action, "on") || !strcmp(action, "off")) return 0;
    int value;
    if (xz_json_get_int(doc, arguments, "value", &value)) return -1;
    if ((!strcmp(action, "brightness") && value >= 1 && value <= 100) ||
        (!strcmp(action, "temperature") && value >= 16 && value <= 30)) {
        snprintf(value_text, 4, "%d", value);
        return 0;
    }
    return -1;
}

/* Only a deployed regular script whose header declares this interface can be
 * selected. Pin the checked inode through exec to avoid an upload race. */
static int open_provider(const char *name) {
    if (!name) return -1;
    size_t n = strlen(name);
    if (n < 4 || n > 80 || name[0] == '.' || strcmp(name + n - 3, ".sh") ||
        strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != n)
        return -1;
    char path[sizeof(XZ_HOME_MCP_APPS) + 81];
    snprintf(path, sizeof(path), "%s/%s", XZ_HOME_MCP_APPS, name);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    char head[1025];
    if (!fstat(fd, &st) && S_ISREG(st.st_mode)) {
        ssize_t rd = pread(fd, head, sizeof(head) - 1, 0);
        if (rd > 0) {
            head[rd] = '\0';
            if (strstr(head, "\n# LX06-HOME-MCP: 1\n")) return fd;
        }
    }
    close(fd);
    return -1;
}

void xz_home_mcp_configure(xz_home_mcp *state, const char *provider) {
    int fd = open_provider(provider);
    if (fd < 0) return;
    close(fd);
    state->provider = provider;
}

static int start_request(xz_home_mcp *state, int socket_fd, char *action,
                         char *device, char *value, const char *id, uint64_t now) {
    int provider = open_provider(state->provider);
    if (provider < 0) return -1;
    char executable[32];
    snprintf(executable, sizeof(executable), "/proc/self/fd/%d", provider);
    int pipefd[2];
    if (pipe(pipefd)) {
        close(provider);
        return -1;
    }
    if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK)) {
        close(provider);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        close(socket_fd);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipefd[1]);
        char *const argv[] = {"sh", executable, "--home-mcp-action", action, device, value, NULL};
        char *const env[] = {"PATH=/usr/bin:/bin", "LD_LIBRARY_PATH=/usr/lib", NULL};
        execve("/bin/sh", argv, env);
        _exit(127);
    }
    close(provider);
    close(pipefd[1]);
    if (pid < 0) {
        close(pipefd[0]);
        return -1;
    }
    state->pid = pid;
    state->fd = pipefd[0];
    state->deadline = now + 4500;
    state->next_call = now + 1200;
    state->used = 0;
    strcpy(state->device, device);
    strcpy(state->action, action);
    strcpy(state->value, value);
    strcpy(state->id, id);
    state->requested_at = now;
    state->from_voice = !*id;
    state->accepted = 0;
    return 0;
}

static int same_action(const xz_home_mcp *state, const char *device, const char *action,
                       const char *value, uint64_t now) {
    return now - state->requested_at < HOME_DUPLICATE_MS && !strcmp(state->device, device) &&
           !strcmp(state->action, action) && !strcmp(state->value, value);
}

static int action_result(xz_ws *ws, const char *session, const char *id, int accepted) {
    if (!*id) return 0;
    return send_result(ws, session, id, "result", accepted
        ? "{\"content\":[{\"type\":\"text\",\"text\":\"家庭服务已受理，设备实际状态未确认。\"}],\"isError\":false}"
        : "{\"content\":[{\"type\":\"text\",\"text\":\"家庭服务受理未确认，请检查接入脚本和联网状态。\"}],\"isError\":true}");
}

static int starts_with(const char *text, const char *prefix) {
    return !strncmp(text, prefix, strlen(prefix));
}

static int ends_with(const char *text, const char *suffix) {
    size_t n = strlen(text), k = strlen(suffix);
    return n >= k && !strcmp(text + n - k, suffix);
}

/* Some gateways answer a device utterance without calling MCP. Use the final
 * STT for single, explicit on/off requests, with no general command passthrough. */
int xz_home_mcp_voice(xz_home_mcp *state, xz_ws *ws, const char *text, uint64_t now) {
    if (!state->provider || state->pid || now < state->next_call) return 0;
    while (*text == ' ') ++text;
    static const char *const polite[] = {"请帮我", "帮我", "请"};
    for (size_t i = 0; i < sizeof(polite) / sizeof(polite[0]); ++i) {
        if (starts_with(text, polite[i])) {
            text += strlen(polite[i]);
            break;
        }
    }
    char utterance[118];
    size_t n = strlen(text);
    if (!n || n >= sizeof(utterance)) return 0;
    memcpy(utterance, text, n + 1);
    while (n && (utterance[n-1] == ' ' || utterance[n-1] == '.' || utterance[n-1] == '!'))
        utterance[--n] = '\0';
    if (ends_with(utterance, "。") || ends_with(utterance, "！"))
        utterance[n - strlen("。")] = '\0';
    static const struct { const char *word; char action[4]; } verbs[] = {
        {"打开", "on"}, {"开启", "on"}, {"拉开", "on"}, {"关上", "off"},
        {"关闭", "off"}, {"关掉", "off"}, {"拉上", "off"},
    };
    char *name = NULL;
    char action[4] = "";
    for (size_t i = 0; i < sizeof(verbs) / sizeof(verbs[0]); ++i) {
        const char *word = verbs[i].word;
        if (starts_with(utterance, word)) name = utterance + strlen(word);
        else if (starts_with(utterance, "把") && ends_with(utterance, word)) {
            utterance[strlen(utterance) - strlen(word)] = '\0';
            name = utterance + strlen("把");
        }
        if (name) {
            strcpy(action, verbs[i].action);
            break;
        }
    }
    if (!name) return 0;
    while (*name == ' ') ++name;
    n = strlen(name);
    while (n && name[n-1] == ' ') name[--n] = '\0';
    if (n > 96 || !device_name(name)) return 0;
    static const char *const types[] = {"窗帘", "灯", "风扇", "空调", "净化器", "加湿器", "电视",
                                        "投影仪", "插座", "扫地机器人", "音响"};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        if (!ends_with(name, types[i])) continue;
        if (same_action(state, name, action, "", now)) return 0;
        char value[] = "";
        return start_request(state, ws->fd, action, name, value, "", now) ? -1 : 1;
    }
    return 0;
}

int xz_home_mcp_request(xz_home_mcp *state, xz_ws *ws, const char *session,
                     const char *message, size_t length, uint64_t now, int closing) {
    if (!state->provider || length > 4096) return 0;
    xz_json_doc doc;
    char id[96], method[40], version[8], envelope_session[96];
    if (xz_json_parse(&doc, message, length)) return 0;
    if (xz_json_object_get(&doc, doc.root, "session_id") >= 0 &&
        (xz_json_get_string(&doc, doc.root, "session_id", envelope_session, sizeof(envelope_session)) ||
         (*envelope_session && strcmp(session, envelope_session)))) return 0;
    int payload = xz_json_object_get(&doc, doc.root, "payload");
    if (rpc_id(&doc, payload, id, sizeof(id)) ||
        xz_json_get_string(&doc, payload, "jsonrpc", version, sizeof(version)) ||
        strcmp(version, "2.0") ||
        xz_json_get_string(&doc, payload, "method", method, sizeof(method))) return 0;
    int params = xz_json_object_get(&doc, payload, "params");
    if (!strcmp(method, "initialize")) {
        return send_result(ws, session, id, "result",
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
            "\"serverInfo\":{\"name\":\"LX06 Smart Home\",\"version\":\"1.0\"}}");
    }
    if (!strcmp(method, "tools/list")) return send_result(ws, session, id, "result", list);
    if (!strcmp(method, "ping")) return send_result(ws, session, id, "result", "{}");
    if (strcmp(method, "tools/call"))
        return send_result(ws, session, id, "error", "{\"code\":-32601,\"message\":\"Method not found\"}");
    char name[48], device[97], action[24], value[4];
    if (xz_json_get_string(&doc, params, "name", name, sizeof(name)) || strcmp(name, HOME_TOOL) ||
        decode_action(&doc, xz_json_object_get(&doc, params, "arguments"), device, action, value))
        return send_result(ws, session, id, "error", "{\"code\":-32602,\"message\":\"无效的设备名或操作\"}");
    if (!closing && state->from_voice && same_action(state, device, action, value, now)) {
        if (!state->pid) return action_result(ws, session, id, state->accepted);
        if (!*state->id) {
            strcpy(state->id, id);
            return 0;
        }
    }
    if (closing || state->pid || now < state->next_call)
        return send_result(ws, session, id, "error", "{\"code\":-32000,\"message\":\"当前无法接收新的设备操作\"}");
    if (start_request(state, ws->fd, action, device, value, id, now))
        return send_result(ws, session, id, "error", "{\"code\":-32001,\"message\":\"智能家居脚本不可用\"}");
    return 0;
}

void xz_home_mcp_close(xz_home_mcp *state) {
    if (!state->pid) return;
    kill(state->pid, SIGKILL);
    while (waitpid(state->pid, NULL, 0) < 0 && errno == EINTR) {}
    close(state->fd);
    state->pid = 0;
}

int xz_home_mcp_poll(xz_home_mcp *state, xz_ws *ws, const char *session, uint64_t now) {
    if (!state->pid) return 0;
    /* The pipe is small and nonblocking. Poll in the voice loop while ubus waits. */
    size_t room = sizeof(state->reply) - state->used;
    if (room) {
        ssize_t n = read(state->fd, state->reply + state->used, room);
        if (n > 0) state->used += (size_t)n;
    }
    int status, accepted = 0;
    pid_t done = waitpid(state->pid, &status, WNOHANG);
    if (!done && now < state->deadline) return 0;
    if (done < 0 && errno == EINTR) return 0;
    if (done > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        state->used < sizeof(state->reply)) {
        /* The child may have written between our first read and waitpid. */
        ssize_t n = read(state->fd, state->reply + state->used, sizeof(state->reply) - state->used);
        if (n >= 0 && state->used + (size_t)n < sizeof(state->reply)) {
            xz_json_doc doc;
            int code;
            accepted = !xz_json_parse(&doc, state->reply, state->used + (size_t)n) &&
                       !xz_json_get_int(&doc, doc.root, "code", &code) && code == 0;
        }
    }
    if (done) {
        close(state->fd);
        state->pid = 0;
    } else xz_home_mcp_close(state);
    state->accepted = accepted;
    return action_result(ws, session, state->id, accepted);
}
