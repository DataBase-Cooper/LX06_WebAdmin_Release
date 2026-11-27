#include "../greeting_load.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "load check failed at line %d\n", __LINE__); return 1; } } while (0)
static void le(unsigned char *p, unsigned n, unsigned value) { for (unsigned i=0;i<n;i++) p[i]=(unsigned char)(value>>(i*8)); }
static void wave(unsigned char *w) {
    memset(w,0,48); memcpy(w,"RIFF",4); le(w+4,4,40); memcpy(w+8,"WAVEfmt ",8);
    le(w+16,4,16); le(w+20,2,1); le(w+22,2,1); le(w+24,4,16000);
    le(w+28,4,32000); le(w+32,2,2); le(w+34,2,16); memcpy(w+36,"data",4);
    le(w+40,4,4); w[44]=1; w[45]=2; w[46]=3; w[47]=4;
}
int main(void) {
    const unsigned char text[]="fixture greeting";
    dc_greeting_identity requested={0}, cached;
    requested.version=1; requested.provider[0]=1; requested.account[0]=2; requested.voice[0]=3;
    requested.epoch=1; requested.voice_revision=7; requested.profile_revision=2;
    requested.rate=16000; requested.channels=1; requested.sample_format=DC_GREETING_S16LE;
    requested.text=text; requested.text_len=sizeof(text)-1; cached=requested;
    unsigned char wav[48], out[8]; size_t n; wave(wav); memset(out,0x55,sizeof out); n=99;
    CHECK(dc_greeting_load(&requested,&cached,wav,sizeof wav,out,sizeof out,&n)==1);
    CHECK(n==4 && !memcmp(out,wav+44,4));
    cached.voice_revision++; n=99; memset(out,0x55,sizeof out);
    CHECK(!dc_greeting_load(&requested,&cached,wav,sizeof wav,out,sizeof out,&n));
    CHECK(n==0 && out[0]==0x55); cached=requested;
    cached.text=(const unsigned char *)"fixture greetinG";
    CHECK(!dc_greeting_load(&requested,&cached,wav,sizeof wav,out,sizeof out,&n)); cached=requested;
    CHECK(!dc_greeting_load(&requested,&cached,wav,47,out,sizeof out,&n));
    CHECK(!dc_greeting_load(&requested,&cached,wav,sizeof wav,out,3,&n));
    wav[22]=2; CHECK(!dc_greeting_load(&requested,&cached,wav,sizeof wav,out,sizeof out,&n)); wave(wav);
    wav[40]=3; CHECK(!dc_greeting_load(&requested,&cached,wav,sizeof wav,out,sizeof out,&n)); wave(wav);
    wav[4]=39; CHECK(!dc_greeting_load(&requested,&cached,wav,sizeof wav,out,sizeof out,&n)); wave(wav);
    CHECK(!dc_greeting_load(NULL,&cached,wav,sizeof wav,out,sizeof out,&n));
    CHECK(!dc_greeting_load(&requested,&cached,NULL,sizeof wav,out,sizeof out,&n));
    CHECK(!dc_greeting_load(&requested,&cached,wav,sizeof wav,NULL,sizeof out,&n));
    CHECK(!dc_greeting_load(&requested,&cached,wav,sizeof wav,out,sizeof out,NULL));
    for (size_t i=0;i<44;i++) {
        wave(wav); wav[i]^=1; n=99; memset(out,0x55,sizeof out);
        CHECK(!dc_greeting_load(&requested,&cached,wav,sizeof wav,out,sizeof out,&n));
        CHECK(n==0 && out[0]==0x55);
    }
    wave(wav); CHECK(dc_greeting_load(&requested,&cached,wav,sizeof wav,out,sizeof out,&n));
    wav[44]=99; CHECK(out[0]==1); /* output is an owned snapshot */
    {
        size_t max=DC_GREETING_WAV_MAX;
        unsigned char *large=calloc(1,max+2), *copy=malloc(max);
        CHECK(large && copy); wave(large);
        le(large+4,4,(unsigned)max-8); le(large+40,4,(unsigned)max-44);
        CHECK(dc_greeting_load(&requested,&cached,large,max,copy,max,&n));
        CHECK(n==max-44);
        le(large+4,4,(unsigned)max-6); le(large+40,4,(unsigned)max-42);
        CHECK(!dc_greeting_load(&requested,&cached,large,max+2,copy,max,&n));
        free(large); free(copy);
    }
    puts("greeting load seam: PASS"); return 0;
}
