/* Independent phone transport candidate. It owns no audio device, recognizer,
 * session policy, TTS or lights. Do not connect it to the core until command_ack
 * and phone_status (TEXT, generation=sequence=0) have a metadata-only consumer.
 * A successful probe is deliberately never translated into dialogue READY. */
#ifndef LX06_DC_PHONE_H
#define LX06_DC_PHONE_H
/* Run one session using an inherited connected Unix SOCK_SEQPACKET descriptor.
 * config_path is an absolute private root-owned 0600 JSON pairing file. No
 * endpoint/token command-line switches or insecure TLS fallback are provided.
 * Returns 0 on CLOSE/peer close, 1 on transport failure, 2 on invalid input. */
int dc_phone_run(int fd, const char *config_path);
/* Original normal main capture loan; all permission remains with the current
 * retained profile/started GRANT and eventual READY. No recognizer, producer, player or queue is opened by
 * this API. A close/revoke permanently ends this operation's capture loan. */
#include <stdint.h>
struct dc_phone_runtime;
struct dc_config;
/* Apply only the retained App BARGEIN word-list override to the existing
 * configuration before opening the original session. Blank/no CTL inherits
 * every field unchanged. Source lifetime/current checks stay with its owner. */
int dc_phone_runtime_configure(struct dc_phone_runtime *,struct dc_config *);
struct dc_io_capture_frame;
int dc_phone_runtime_microphone_allowed(struct dc_phone_runtime *);
void dc_phone_runtime_microphone_revoke(struct dc_phone_runtime *);
int dc_phone_runtime_microphone(struct dc_phone_runtime *,const struct dc_io_capture_frame *);
#endif
