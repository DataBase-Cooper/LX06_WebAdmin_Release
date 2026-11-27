/* Private in-process owner composition for the original speaker HTTP path.
 * No new CLI, socket protocol, account store, worker or background service. */
#ifndef LX06_MODEL_HTTP_OWNED_H
#define LX06_MODEL_HTTP_OWNED_H
#ifndef LX06_MODEL_HTTP_OWNED_FIXTURE
#error "owned model HTTP composition is a private integration candidate"
#endif
#include <stddef.h>
#include <stdint.h>
#define MHO_BYTES 8192U
#define MHO_ID_BYTES 96U
struct mho_client;
typedef int (*mho_owner_current)(void *,uint64_t,uint64_t);
/* Serial owner only. current may cancel, but cannot free this borrowed client.
 * ca/allow_http are explicit private fixture options; default HTTPS validates
 * the original certificate chain and hostname. No ambient credential reads. */
struct mho_client *mho_create(mho_owner_current,void *,uint64_t,uint64_t,const char *ca,int allow_http);
int mho_destroy(struct mho_client *);
int mho_current(struct mho_client *,uint64_t,uint64_t);
uint64_t mho_now(struct mho_client *);
/* Original credential/config owner callback only; synchronous and borrowed.
 * Destroy, current, post and recursive owner calls on this SAME client are
 * refused and latch invalidity until outer return. Exact cancel and numeric
 * monotonic observation remain available. No callback may replace the pointer
 * or mutate the adapter/borrowed request. Returns callback result, or -1 on an
 * invalid borrow. After return the owner may retry destruction. */
int mho_owner_call(struct mho_client *,int (*callback)(void *),void *context);
/* Headers are the existing owner's curl-config subset, copied only in RAM.
 * body/result limits include the actual HTTP JSON envelope, without truncation.
 * http_status is numeric diagnostics; error bodies are never returned. */
int mho_post(struct mho_client *,const char *request_id,const char *url,
             const char *headers,const char *body,size_t,unsigned timeout_ms,
             uint64_t absolute_deadline,char [MHO_BYTES+1],size_t *,long *http_status);
void mho_cancel(struct mho_client *,const char *request_id);
#endif
