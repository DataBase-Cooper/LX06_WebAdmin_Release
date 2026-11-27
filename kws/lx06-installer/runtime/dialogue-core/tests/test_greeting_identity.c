#include "../greeting_identity.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"identity check failed at line %d\n", __LINE__); return 1; } } while (0)

int main(void) {
    const unsigned char text[] = "fixture greeting";
    dc_greeting_identity a = {0}, b;
    a.version = 1; a.epoch = 7; a.voice_revision = 3; a.profile_revision = 2;
    a.provider[0] = 1; a.account[0] = 2; a.voice[0] = 3;
    a.rate = 16000; a.channels = 1; a.sample_format = DC_GREETING_S16LE;
    a.text = text; a.text_len = sizeof(text)-1;
    b = a;
    CHECK(dc_greeting_identity_match(&a, &b) == 1);
    { unsigned char equal_text[sizeof text]; memcpy(equal_text,text,sizeof text);
      b.text=equal_text; CHECK(dc_greeting_identity_match(&a,&b)); b=a; }
    { unsigned char long_text[240]; memset(long_text,'x',sizeof long_text);
      b.text=long_text; b.text_len=sizeof long_text; CHECK(dc_greeting_identity_match(&b,&b)); b=a; }
    CHECK(dc_greeting_identity_match(NULL, &b) == 0);
    b.version = 2; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.epoch++; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.voice_revision++; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.profile_revision++; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.provider[31]=1; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.account[31]=1; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.voice[31]=1; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.rate=24000; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.channels=2; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.sample_format=0; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.text=(const unsigned char *)"fixture greetinG"; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.text_len--; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.text=NULL; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.text_len=0; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.text_len=241; CHECK(!dc_greeting_identity_match(&a,&b)); b=a;
    b.text=(const unsigned char *)"x\0y"; b.text_len=3; CHECK(!dc_greeting_identity_match(&b,&b)); b=a;
    memset(b.account,0,sizeof b.account); CHECK(!dc_greeting_identity_match(&b,&b)); b=a;
    memset(b.provider,0,sizeof b.provider); CHECK(!dc_greeting_identity_match(&b,&b)); b=a;
    memset(b.voice,0,sizeof b.voice); CHECK(!dc_greeting_identity_match(&b,&b)); b=a;
    b.epoch=0; CHECK(!dc_greeting_identity_match(&b,&b)); b=a;
    b.voice_revision=0; CHECK(!dc_greeting_identity_match(&b,&b)); b=a;
    b.profile_revision=0; CHECK(!dc_greeting_identity_match(&b,&b));
    puts("greeting identity contract: PASS");
    return 0;
}
