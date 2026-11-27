#include "../src/echo.h"

#include <assert.h>
#include <string.h>

static void signal_frame(int16_t *pcm,unsigned count,unsigned repeat,
                         uint32_t seed,int amplitude) {
    for(unsigned i=0;i<count;i+=repeat) {
        seed=1664525u*seed+1013904223u;
        for(unsigned j=0;j<repeat;++j) pcm[i+j]=(seed>>31)?amplitude:-amplitude;
    }
}

static int silent(const unsigned char *pcm) {
    for(unsigned i=0;i<DB_ECHO_FRAME_BYTES;++i) if(pcm[i]) return 0;
    return 1;
}

int main(void) {
    db_echo echo={.lock=PTHREAD_MUTEX_INITIALIZER,.speech_rms=DB_BARGE_DEFAULT_RMS};
    db_echo sensitive={.lock=PTHREAD_MUTEX_INITIALIZER,.speech_rms=DB_BARGE_MIN_RMS,.strong_protection=1};
    int16_t far[480],reflected[320],lead[320],near[320];
    unsigned char out[DB_ECHO_FRAME_BYTES];
    signal_frame(far,480,6,7,2300);
    signal_frame(reflected,320,4,7,2300);
    signal_frame(lead,320,4,13,600);
    signal_frame(near,320,4,42,1080);

    /* Partial pipe writes still form the same reference PCM. Even a loud
     * external echo above the level gate must not open native interruption. */
    const unsigned char *wire=(const unsigned char *)far;
    db_echo_reference(&echo,wire,481,1000);
    db_echo_reference(&echo,wire+481,sizeof(far)-481,1001);
    echo.speech_rms=4000;
    assert(db_echo_keyword_filter(&echo,(unsigned char *)reflected,1020,out));
    assert(silent(out));
    assert(!db_echo_keyword_filter(&echo,(unsigned char *)lead,1020,out));
    echo.speech_rms=DB_BARGE_DEFAULT_RMS;
    for(unsigned i=0;i<5;++i) {
        assert(db_echo_filter(&echo,(unsigned char *)reflected,1,1020+20*i,out));
        assert(silent(out));
    }

    db_echo_reference(&sensitive,wire,sizeof(far),1000);
    for(unsigned i=0;i<5;++i) {
        assert(db_echo_filter(&sensitive,(unsigned char *)reflected,1,1020+20*i,out));
        assert(silent(out));
    }
    for(unsigned i=0;i<3;++i)
        assert(db_echo_filter(&sensitive,(unsigned char *)lead,1,1120+20*i,out));
    assert(!db_echo_filter(&sensitive,(unsigned char *)lead,1,1180,out));
    assert(!memcmp(out,lead,sizeof(lead)));
    assert(!pthread_mutex_destroy(&sensitive.lock));

    assert(db_echo_filter(&echo,(unsigned char *)lead,1,1120,out));
    assert(db_echo_filter(&echo,(unsigned char *)near,1,1140,out));
    assert(db_echo_filter(&echo,(unsigned char *)near,1,1160,out));
    assert(silent(out));
    assert(!db_echo_filter(&echo,(unsigned char *)near,1,1180,out));
    assert(!memcmp(out,lead,sizeof(lead)));
    assert(!db_echo_filter(&echo,(unsigned char *)near,0,1200,out));
    assert(!memcmp(out,near,sizeof(near)));

    /* An ended reply leaves no audible echo in the ordinary listening path. */
    assert(db_echo_filter(&echo,(unsigned char *)lead,1,1800,out));
    assert(!db_echo_filter(&echo,(unsigned char *)lead,0,1820,out));
    assert(!db_echo_filter(&echo,(unsigned char *)lead,0,1840,out));
    assert(!db_echo_filter(&echo,(unsigned char *)lead,0,1860,out));
    assert(silent(out));
    assert(!db_echo_filter(&echo,(unsigned char *)lead,0,1880,out));
    assert(!memcmp(out,lead,sizeof(lead)));

    return pthread_mutex_destroy(&echo.lock);
}
