#include "script-audio-mode.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const char *cloud = "#!/bin/sh\n# LX06-AUDIO-MODE: cloud-e2e\n";
    const char *phone = "#!/bin/sh\n# LX06-AUDIO-MODE: phone-bridge\r\n# LX06-PHONE-APP: doubao \t\n";
    assert(lx06_script_audio_parse((const unsigned char *)cloud,strlen(cloud)).mode==LX06_AUDIO_CLOUD_E2E);
    struct lx06_script_audio_metadata m=lx06_script_audio_parse((const unsigned char *)phone,strlen(phone));
    assert(m.mode==LX06_AUDIO_PHONE_BRIDGE&&m.phone_app==LX06_PHONE_DOUBAO);
    const char *cases[]={
        "# LX06-AUDIO-MODE: phone-bridge\n",
        "# LX06-PHONE-APP: doubao\n",
        "# LX06-AUDIO-MODE: phone-bridge\n# LX06-PHONE-APP: unknown\n",
        "# LX06-AUDIO-MODE: cloud-e2e\n# LX06-AUDIO-MODE: account-text\n",
        "# LX06-AUDIO-MODE: unknown\n# LX06-AUDIO-MODE: cloud-e2e\n",
        "# LX06-AUDIO-MODE: cloud-e2e; reboot\n",
        "# LX06-AUDIO-MODE: cloud-e2e",
        "# LX06-AUDIO-MODE: phone-bridge\n# LX06-PHONE-APP: doubao\n# LX06-PHONE-APP: grok\n",
    };
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++)
        assert(lx06_script_audio_parse((const unsigned char *)cases[i],strlen(cases[i])).mode==LX06_AUDIO_INVALID);
    unsigned char bounded[8192];memset(bounded,'a',sizeof bounded);
    assert(lx06_script_audio_parse(bounded,sizeof bounded).mode==LX06_AUDIO_UNSPECIFIED);
    memcpy(bounded+8192-27,"# LX06-AUDIO-MODE: cloud-e2e",27);
    assert(lx06_script_audio_parse(bounded,sizeof bounded).mode==LX06_AUDIO_UNSPECIFIED);
    assert(lx06_script_audio_parse(NULL,0).mode==LX06_AUDIO_INVALID);
    puts("script_audio_metadata=passed");
    return 0;
}
