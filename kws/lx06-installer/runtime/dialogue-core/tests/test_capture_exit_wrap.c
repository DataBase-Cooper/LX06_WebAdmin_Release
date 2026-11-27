/* Synthetic device/ASR boundaries; the executable retains production main. */
#include "../io/io.h"
#include "../asr/control_asr.h"
#include "../audio_route.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static int delivered;
dc_io_capture *__wrap_dc_io_capture_open_mode(const char *d,const char *n,enum dc_io_capture_mode m){(void)d;(void)n;(void)m;if(getenv("TEST_MISSING_MIC")){errno=ENOENT;return NULL;}return (dc_io_capture *)&delivered;}
void __wrap_dc_io_capture_close(dc_io_capture *c){(void)c;}
int __wrap_dc_io_capture_fd(const dc_io_capture *c){(void)c;return -1;}
int __wrap_dc_io_capture_stalled(const dc_io_capture *c,uint64_t n){(void)c;(void)n;return 0;}
int __wrap_dc_io_capture_next(dc_io_capture *c,dc_io_capture_frame *f){(void)c;if(delivered++)return 0;if(getenv("TEST_GENERIC"))return -EIO;memset(f,0,sizeof *f);f->rate=16000;f->bytes=320;f->mode=DC_IO_CAPTURE_LXA1;return 1;}
lx06_control_asr *__wrap_lx06_control_asr_open_profile(const char *s,enum lx06_asr_profile p){(void)s;(void)p;return (lx06_control_asr *)&delivered;}
void __wrap_lx06_control_asr_close(lx06_control_asr *a){(void)a;}
void __wrap_lx06_control_asr_reply(lx06_control_asr *a,uint64_t n){(void)a;(void)n;}
void __wrap_lx06_control_asr_mic_pending(lx06_control_asr *a,size_t n){(void)a;(void)n;}
int __wrap_lx06_control_asr_feed_stamped(lx06_control_asr *a,int p,const void *v,size_t n,const struct qev_stamp *s,uint64_t t){(void)a;(void)p;(void)v;(void)n;(void)s;(void)t;return 0;}
uint64_t __wrap_lx06_control_asr_active_serial(const lx06_control_asr *a){(void)a;return 1;}
int __wrap_dc_audio_route_capture(dc_audio_route *r,const void *p,size_t n,uint64_t u,int v,uint64_t t){(void)r;(void)p;(void)n;(void)u;(void)v;(void)t;return getenv("TEST_ROUTE_OTHER")?-EIO:-ENOBUFS;}
