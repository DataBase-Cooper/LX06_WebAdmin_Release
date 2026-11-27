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
    "device 填设备名，可带房间，例如客厅灯。工具返回成功时，请直接肯定地回复完成，"
    "无需查询设备，也不要回答不知道、无法控制或不确定。\","
    "\"inputSchema\":{\"type\":\"object\",\"required\":[\"device\",\"action\"],"
    "\"additionalProperties\":false,\"properties\":{"
    "\"device\":{\"type\":\"string\",\"description\":\"设备名或房间加设备名\"},"
    "\"action\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"brightness\",\"temperature\","
    "\"mode\",\"light_color\",\"light_mode\"]},"
    "\"value\":{\"type\":\"integer\",\"description\":\"亮度百分比或空调摄氏度，仅调节时填写\"},"
    "\"mode\":{\"type\":\"string\",\"enum\":[\"cool\",\"heat\",\"auto\",\"dry\",\"fan\",\"moonlight\",\"highlight\"],"
    "\"description\":\"mode 用于空调制冷、制热、自动、除湿和送风。light_mode 用于灯光的月光或高亮模式。\"},"
    "\"color\":{\"type\":\"string\",\"enum\":[\"white\",\"yellow\"],"
    "\"description\":\"light_color 使用 white 白色或 yellow 黄色\"}}}}]}";

static const char *const types[] = {"窗帘", "灯", "灯光", "灯泡", "灯带", "风扇", "空调", "空气净化器", "净化器",
                                    "加湿器", "电视", "投影仪", "插座", "扫地机器人", "音响"};
static const struct { const char *word, *value, *action; } presets[] = {
    {"制冷", "cool", "mode"}, {"制热", "heat", "mode"}, {"自动", "auto", "mode"},
    {"除湿", "dry", "mode"}, {"送风", "fan", "mode"},
    {"白色", "white", "light_color"}, {"白光", "white", "light_color"},
    {"黄色", "yellow", "light_color"}, {"黄光", "yellow", "light_color"},
    {"月光", "moonlight", "light_mode"}, {"高亮", "highlight", "light_mode"},
};

static int starts_with(const char *text, const char *prefix) {
    return !strncmp(text, prefix, strlen(prefix));
}

static int ends_with(const char *text, const char *suffix) {
    size_t n = strlen(text), k = strlen(suffix);
    return n >= k && !strcmp(text + n - k, suffix);
}

static int preset_applies(const char *action, const char *device) {
    if (!strcmp(action, "mode")) return ends_with(device, "空调");
    return ends_with(device, "灯") || ends_with(device, "灯光") ||
           ends_with(device, "灯泡") || ends_with(device, "灯带");
}

/* The STT can say 客厅的空调 while a cloud tool uses 客厅空调 for the same target. */
static int normalize_device(char *name) {
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        if (!ends_with(name, types[i])) continue;
        size_t prefix = strlen(name) - strlen(types[i]);
        size_t particle = strlen("的");
        if (prefix > particle && !memcmp(name + prefix - particle, "的", particle))
            memmove(name + prefix - particle, name + prefix, strlen(types[i]) + 1);
        return 1;
    }
    return 0;
}

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
                         char action[24], char value_text[XZ_HOME_VALUE_CAP]) {
    if (xz_json_get_string(doc, arguments, "device", device, 97) ||
        xz_json_get_string(doc, arguments, "action", action, 24) ||
        !device_name(device)) return -1;
    (void)normalize_device(device);
    value_text[0] = '\0';
    if (!strcmp(action, "on") || !strcmp(action, "off")) return 0;
    if (!strcmp(action, "mode") || !strcmp(action, "light_mode") || !strcmp(action, "light_color")) {
        const char *field = !strcmp(action, "light_color") ? "color" : "mode";
        if (!preset_applies(action, device) ||
            xz_json_get_string(doc, arguments, field, value_text, XZ_HOME_VALUE_CAP))
            return -1;
        for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); ++i)
            if (!strcmp(action, presets[i].action) && !strcmp(value_text, presets[i].value)) return 0;
        return -1;
    }
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
    state->next_call = now + 250;
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
        ? "{\"content\":[{\"type\":\"text\",\"text\":\"操作成功。请直接用自己的话简短确认已完成，无需再查询设备，不要说不知道、无法控制或不确定。\"}],\"isError\":false}"
        : "{\"content\":[{\"type\":\"text\",\"text\":\"小爱未受理此次请求。\"}],\"isError\":true}");
}

static char *voice_preset(char *text, char action[24], char value[XZ_HOME_VALUE_CAP]) {
    static const char *const verbs[] = {"调整成", "调整到", "调整为", "调到", "调成", "调为",
                                        "切换到", "切换为", "切换成", "设为", "设置为", "设置成",
                                        "改成", "换成", "进入", "开启", "打开", "开"};
    if (starts_with(text, "把") || starts_with(text, "将") || starts_with(text, "给"))
        text += strlen("把");
    if (starts_with(text, "如何") || starts_with(text, "怎么") || starts_with(text, "能不能")) return NULL;
    for (size_t i = 0; i < sizeof(verbs) / sizeof(verbs[0]); ++i) {
        char *split = strstr(text, verbs[i]);
        if (!split) continue;
        const char *setting = split + strlen(verbs[i]);
        for (size_t j = 0; j < sizeof(presets) / sizeof(presets[0]); ++j) {
            const char *word = presets[j].word;
            if (!starts_with(setting, word)) continue;
            const char *suffix = setting + strlen(word);
            if (*suffix && strcmp(suffix, "模式")) continue;
            char delimiter = *split;
            *split = '\0';
            if (!preset_applies(presets[j].action, text)) {
                *split = delimiter;
                return NULL;
            }
            strcpy(action, presets[j].action);
            strcpy(value, presets[j].value);
            return text;
        }
    }
    return NULL;
}

/* Some gateways answer a device utterance without calling MCP. Use the final
 * STT for explicit single-device switches and supported presets. */
int xz_home_mcp_voice(xz_home_mcp *state, xz_ws *ws, const char *text, uint64_t now) {
    if (!state->provider) return 0;
    while (*text == ' ') ++text;
    static const char *const polite[] = {"请帮我", "帮我", "请给我", "给我", "请"};
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
        {"关闭", "off"}, {"关掉", "off"}, {"拉上", "off"}, {"开一下", "on"},
        {"关一下", "off"}, {"关了", "off"}, {"开", "on"}, {"关", "off"},
    };
    char value[XZ_HOME_VALUE_CAP] = "", action[24] = "";
    char *name = voice_preset(utterance, action, value);
    for (size_t i = 0; !name && i < sizeof(verbs) / sizeof(verbs[0]); ++i) {
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
    if (n > 96 || !device_name(name) || !normalize_device(name)) return 0;
    if (same_action(state, name, action, value, now)) return 2;
    if (state->pid || now < state->next_call) return 0;
    return start_request(state, ws->fd, action, name, value, "", now) ? -1 : 1;
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
    char name[48], device[97], action[24], value[XZ_HOME_VALUE_CAP];
    if (xz_json_get_string(&doc, params, "name", name, sizeof(name)) || strcmp(name, HOME_TOOL) ||
        decode_action(&doc, xz_json_object_get(&doc, params, "arguments"), device, action, value))
        return send_result(ws, session, id, "error", "{\"code\":-32602,\"message\":\"无效的设备名或操作\"}");
    if (!closing && state->from_voice && same_action(state, device, action, value, now)) {
        if (!state->pid) return action_result(ws, session, id, state->accepted);
        if (!*state->id) {
            strcpy(state->id, id);
            return 0;
        }
        if (!strcmp(state->id, id)) return 0;
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
