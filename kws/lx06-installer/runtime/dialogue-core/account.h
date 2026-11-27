/* Account text adapter for the unified dialogue controller.
 *
 * This module owns one script request, bounded NDJSON/text and confirmed
 * history. It has no microphone, ASR, player, light or control-word policy.
 * All identities are allocated by the controller. Every function is
 * nonblocking; call tick/poll regularly even when playback is busy.
 */
#ifndef LX06_DC_ACCOUNT_H
#define LX06_DC_ACCOUNT_H
#include "types.h"
#include <stddef.h>
#include <stdint.h>

#define DC_ACCOUNT_QUESTION_CAP 2048u
#define DC_ACCOUNT_ANSWER_CAP 16385u
#define DC_ACCOUNT_DELTA_CAP 2049u
#define DC_ACCOUNT_PHRASE_CAP 241u
#define DC_ACCOUNT_WIRE_CAP 32768u
#define DC_ACCOUNT_LINE_CAP 4096u
#define DC_ACCOUNT_HISTORY_PAIRS 3u
#define DC_ACCOUNT_DECLARATION_INPUT_CAP (1024u * 1024u)

/* Pure byte parser ONLY: it does not authenticate, snapshot or execute a
 * script, bind a session, or override runtime settings. Caller must supply the
 * exact complete immutable bytes that will execute before using this result.
 * v1 requires exactly the first three LF-terminated lines: #!/bin/sh,
 * identity v1, and interruption keyword or voice. The lexical header is the
 * initial run of blank and comment-like lines; it ends at the first other
 * line, including a shell command, assignment, quote or heredoc introducer.
 * Only the header is searched for declaration-like tags, so body text is not
 * interpreted as metadata. ASCII whitespace and any high byte are treated as
 * possible prefixes before '#', or between '#' and a tag, to reject disguised
 * declarations conservatively. Malformed, duplicated or misplaced tags within
 * that header, embedded NUL anywhere and oversized/empty input are invalid.
 * No tag in the header returns undeclared legacy, including tags in the body.
 * Return -1 invalid, 0 undeclared legacy, 1 keyword, 2 voice. */
enum dc_account_declaration { DC_ACCOUNT_LEGACY=0, DC_ACCOUNT_KEYWORD=1, DC_ACCOUNT_VOICE=2 };
int dc_account_parse_declaration(const void *bytes, size_t length);

typedef struct dc_account dc_account;
enum dc_account_event_type {
    DC_ACCOUNT_DELTA = 1, DC_ACCOUNT_PHRASE, DC_ACCOUNT_DONE, DC_ACCOUNT_ERROR
};
enum dc_account_error {
    DC_ACCOUNT_ERR_IO = 1, DC_ACCOUNT_ERR_PROTOCOL, DC_ACCOUNT_ERR_LIMIT,
    DC_ACCOUNT_ERR_ADAPTER, DC_ACCOUNT_ERR_TIMEOUT
};
typedef struct dc_account_event {
    enum dc_account_event_type type;
    dc_tag tag;
    uint64_t segment_sequence;
    uint64_t at_ms;
    enum dc_account_error error;
    size_t length;
    char text[DC_ACCOUNT_DELTA_CAP];
} dc_account_event;

dc_account *dc_account_create(void);
/* Opt in before the first request. fd must already be a fully sealed memfd
 * supplied by a trusted session owner. Returns its declaration (1/2), or -1.
 * The binding is immutable for this account object's lifetime and is limited
 * to epoch. Caller must derive controller barge_any from the returned mode,
 * never from settings. This API does not authenticate the fd's supplier. */
int dc_account_bind_sealed_v1(dc_account *, uint64_t epoch, int fd);
/* Returns EBUSY until active/cancelled children have been reaped. It never
 * sleeps or hides a wait in controller shutdown. */
int dc_account_destroy(dc_account *);
/* question must be a complete genuine ASR FINAL with controls already removed.
 * The script retains its own provider, model choice, account and HTTP pool env.
 * The request goes to /bin/sh script --model-request on a private stdin pipe.
 * One epoch contains at most three confirmed (DONE + EOF + exit 0) pairs.
 * Returns -1/EINVAL, EMSGSIZE, EBUSY or a launch error without starting work. */
int dc_account_start(dc_account *, dc_tag, const char *question,
                     const char *script, const char *model, uint64_t now_ms);
/* Read fd and pending stdin-write fd, for optional integration into poll/epoll.
 * Both return -1 when absent. Calling poll every control-loop turn is enough;
 * a writable input pipe is never waited for synchronously. */
int dc_account_fd(const dc_account *);
int dc_account_write_fd(const dc_account *);
/* At most one DELTA/DONE/ERROR event per call, bounded input and parse work.
 * Return 1 event, 0 no event, -1 invalid arguments. DONE means transport was
 * verified, not that all phrases were taken or audio was played. */
int dc_account_poll(dc_account *, uint64_t now_ms, dc_account_event *);
/* Pull one UTF-8 phrase only when the controller has a free TTS slot. No
 * internal event/segment queue grows with the length of an answer. */
int dc_account_phrase(dc_account *, uint64_t now_ms, dc_account_event *);
/* Exact epoch+reply cancellation: discard old text/events first, send TERM,
 * then tick sends KILL after 80 ms and reaps without waiting. Wrong/stale tags
 * do nothing; the controller must silence its player before invoking this. */
int dc_account_cancel(dc_account *, dc_tag, uint64_t now_ms);
void dc_account_tick(dc_account *, uint64_t now_ms);
int dc_account_busy(const dc_account *);
int dc_account_drained(const dc_account *);
unsigned dc_account_retired(const dc_account *);
unsigned dc_account_history_count(const dc_account *);
/* Numeric accounting for resource/fixture reports; never contains text. */
size_t dc_account_storage_bytes(void);
#endif
