#ifndef LX06_DIALOGUE_TRANSPORT_H
#define LX06_DIALOGUE_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
/* Protocol-only transport. The owner chooses every PCM sample, turn, cancel
 * and close. No microphone, player, light, word matcher or session policy. */
enum dc_transport_provider { DC_TRANSPORT_XIAOZHI=1, DC_TRANSPORT_DOUBAO=2 };
enum dc_transport_event_type {
 DC_T_READY=1, DC_T_REPLY_BEGIN, DC_T_PCM, DC_T_ASSISTANT_TEXT,
 DC_T_USER_PARTIAL, DC_T_USER_FINAL, DC_T_SPEECH_STARTED, DC_T_REPLY_END,
 DC_T_CANCEL_ACK, DC_T_LISTEN_READY, DC_T_REMOTE_CLOSE, DC_T_ERROR
};
struct dc_transport_config {
 enum dc_transport_provider provider;
 const char *endpoint,*device_id,*client_id,*credential;
 const char *voice,*instructions,*greeting;
 /* Protocol negotiation only: 0=auto/guarded, 1=realtime/native VAD.
  * This never enables a local control policy or reads a microphone. */
 int native_realtime,output_pcm;
 unsigned connect_timeout_ms;
 /* XZ only; interval0 disables. One exact-token ping can be outstanding. */
 unsigned ping_interval_ms,pong_timeout_ms;
};
struct dc_transport_event {
 enum dc_transport_event_type type;
 /* Monotonic cloud reply sequence, mapped to generation by the core. */
 uint64_t reply_sequence;
 unsigned sample_rate,code;
 const void *data;
 size_t bytes;
};
typedef int (*dc_transport_sink)(void *,const struct dc_transport_event *);
typedef struct dc_transport dc_transport;
dc_transport *dc_transport_open(const struct dc_transport_config *,dc_transport_sink,void *);
int dc_transport_pcm(dc_transport *,const void *,size_t);
int dc_transport_poll(dc_transport *,unsigned timeout_ms);
int dc_transport_cancel(dc_transport *,uint64_t reply_sequence);
int dc_transport_listen(dc_transport *);
int dc_transport_commit(dc_transport *);
int dc_transport_text(dc_transport *,const char *);
int dc_transport_ping(dc_transport *);
void dc_transport_close(dc_transport *);
#endif
