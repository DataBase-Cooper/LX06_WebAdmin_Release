/* This private pipe consumes complete frames from processed-mic-read --kws-epoch. */
#define _GNU_SOURCE
#include <stdio.h>
#include "wake-oem-ns-stream.h"

static int write_frame(const uint8_t *data,size_t size,void *unused) {
    (void)unused;
    return fwrite(data,1,size,stdout)!=size || fflush(stdout);
}
int main(void) {
    static struct wake_ns_stream state;
    uint8_t frame[LX06_WAKE_FRAME_BYTES];
    size_t count;
    int failed=0;
    while((count=fread(frame,1,sizeof(frame),stdin))==sizeof(frame)) {
        if(wake_ns_frame(&state,frame,write_frame,NULL)) { failed=1; break; }
    }
    wake_ns_end(&state);
    return failed || count || ferror(stdin) ? 1 : 0;
}
