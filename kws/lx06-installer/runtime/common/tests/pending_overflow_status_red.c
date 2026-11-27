/* Offline requirements probe: include the selected real translation unit. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include CONTROL_ASR_SOURCE
#ifdef LX06_QUESTION_ENDPOINT_VAD_H
/* No VAD instance is created in this deferred-path fixture. */
qev_endpoint *qev_new(unsigned p) { (void)p; abort(); }
void qev_free(qev_endpoint *p) { (void)p; abort(); }
void qev_begin(qev_endpoint *p,uint64_t n,uint64_t t) { (void)p;(void)n;(void)t;abort(); }
int qev_feed(qev_endpoint *p,const int16_t *x,size_t n,const struct qev_stamp *s,uint64_t t) { (void)p;(void)x;(void)n;(void)s;(void)t;abort(); }
int qev_ready(const qev_endpoint *p,const struct qev_gate *g,uint64_t t) { (void)p;(void)g;(void)t;abort(); }
void qev_stats_read(const qev_endpoint *p,struct qev_stats *s) { (void)p;(void)s;abort(); }
#endif

void *__real_malloc(size_t);
static int fail_bank;
void *__wrap_malloc(size_t n) {
 if (fail_bank && n == 64000) { errno = ENOMEM; return NULL; }
 return __real_malloc(n);
}

int main(int argc, char **argv) {
 if (argc != 3) return 2;
 int profile = atoi(argv[1]), oom = atoi(argv[2]);
 lx06_control_asr *a = calloc(1, sizeof(*a));
 unsigned char chunk[32000];
 if (!a) return 2;
 a->profile = profile; a->ready_after = 1000; a->fd = -1;
 fail_bank = oom;
 size_t capacity = profile && !oom ? 128000 : 64000;
 for (size_t i = 0; i < capacity / sizeof(chunk); i++) {
  memset(chunk, (int)(i + 1), sizeof(chunk));
  if (i == 0) { chunk[0] = 0x42; chunk[1] = 0x43; }
  int rc = lx06_control_asr_feed(a, 1, chunk, sizeof(chunk), 1);
  if (rc || a->pending_used != (i+1)*sizeof(chunk) || a->pending_dropped) {
   fprintf(stderr, "SETUP: exact-capacity feed failed rc=%d used=%zu dropped=%zu\n", rc, a->pending_used, a->pending_dropped); return 2;
  }
 }
 memset(chunk, 0x7f, 2);
 int rc = lx06_control_asr_feed(a, 1, chunk, 2, 1);
 size_t available; unsigned char *oldest = pending_span(a, a->pending_head, &available);
 if (a->pending_dropped && (a->pending_dropped != 2 || *oldest != 1 || oldest[0] != 1)) {
  fprintf(stderr, "SETUP: unexpected eviction contents or count\n"); return 2;
 }
 /* Unique byte marker: next oldest differs from the discarded first sample. */
 if (a->pending_dropped && rc == 0) {
  fprintf(stderr, "RED: cap=%zu evicted=%zu feed_rc=%d\n", capacity, a->pending_dropped, rc);
  free(a->question_pending); free(a); return 1;
 }
 free(a->question_pending); free(a); return 0;
}
