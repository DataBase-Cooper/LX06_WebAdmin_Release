/* Offline contract test; includes an original or scratch-transformed real TU. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include CONTROL_ASR_SOURCE
#ifdef LX06_QUESTION_ENDPOINT_VAD_H
qev_endpoint *qev_new(unsigned p){(void)p;abort();}
void qev_free(qev_endpoint *p){(void)p;abort();}
void qev_begin(qev_endpoint *p,uint64_t n,uint64_t t){(void)p;(void)n;(void)t;abort();}
int qev_feed(qev_endpoint *p,const int16_t *x,size_t n,const struct qev_stamp *s,uint64_t t){(void)p;(void)x;(void)n;(void)s;(void)t;abort();}
int qev_ready(const qev_endpoint *p,const struct qev_gate *g,uint64_t t){(void)p;(void)g;(void)t;abort();}
void qev_stats_read(const qev_endpoint *p,struct qev_stats *s){(void)p;(void)s;abort();}
#endif
void *__real_malloc(size_t);
static int fail_bank;
void *__wrap_malloc(size_t n){if(fail_bank && n==64000){errno=ENOMEM;return NULL;}return __real_malloc(n);}
static int feed(lx06_control_asr *a,const void *p,size_t n){
#ifdef LX06_QUESTION_ENDPOINT_VAD_H
 return lx06_control_asr_feed_stamped(a,1,p,n,NULL,1);
#else
 return lx06_control_asr_feed(a,1,p,n,1);
#endif
}
int main(int argc,char **argv){
 if(argc!=3)return 2;
 int profile=atoi(argv[1]),oom=atoi(argv[2]);
 lx06_control_asr *a=calloc(1,sizeof(*a));if(!a)return 2;
 a->profile=profile;a->ready_after=1000;a->fd=-1;fail_bank=oom;
 size_t capacity=profile&&!oom?128000:64000;
 unsigned char chunk[32000],snapshot[128000],current[960],original[960];
 for(size_t i=0;i<capacity;i+=sizeof chunk){
  for(size_t j=0;j<sizeof chunk;j++)chunk[j]=(unsigned char)((i+j)*37u+((i+j)>>8));
  if(feed(a,chunk,sizeof chunk)){fprintf(stderr,"SETUP feed\n");return 2;}
 }
 for(size_t i=0;i<capacity;i++){size_t avail;unsigned char *p=pending_span(a,(a->pending_head+i)%capacity,&avail);snapshot[i]=*p;}
 for(size_t i=0;i<sizeof current;i++)original[i]=current[i]=(unsigned char)(i*13u+7u);
 size_t head=a->pending_head,used=a->pending_used,dropped=a->pending_dropped;
 errno=0;int rc=feed(a,current,sizeof current);int e=errno;
 if(rc!=-2 || e!=ENOBUFS){fprintf(stderr,"RED: expected dedicated overflow rc=-2 errno=ENOBUFS, got rc=%d errno=%d dropped=%zu\n",rc,e,a->pending_dropped);return 1;}
 if(a->pending_head!=head||a->pending_used!=used||a->pending_dropped!=dropped||memcmp(current,original,sizeof current)){fprintf(stderr,"RED: queue metadata or current frame changed\n");return 1;}
 for(size_t i=0;i<capacity;i++){size_t avail;unsigned char *p=pending_span(a,(a->pending_head+i)%capacity,&avail);if(*p!=snapshot[i]){fprintf(stderr,"RED: queued byte changed at %zu\n",i);return 1;}}
 free(a->question_pending);free(a);puts("PASS: dedicated error and exact queued/current bytes retained by caller");return 0;
}
