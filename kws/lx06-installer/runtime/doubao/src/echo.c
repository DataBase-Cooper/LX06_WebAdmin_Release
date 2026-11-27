#include "echo.h"

#include <string.h>

enum {
    MATCH_SAMPLES=80, REFERENCE_LIFETIME_MS=600,
    SPEECH_HOLD_MS=480
};

static int16_t sample(const unsigned char *pcm) {
    int16_t value;
    memcpy(&value,pcm,sizeof(value));
    return value;
}

static void append(db_echo *echo,int16_t value,unsigned decimation) {
    echo->sum+=value;
    if(++echo->phase!=decimation) return;
    echo->reference[echo->head]=(int16_t)(echo->sum/(int)decimation);
    echo->head=(echo->head+1)%DB_ECHO_REFERENCE;
    if(echo->used<DB_ECHO_REFERENCE) ++echo->used;
    echo->phase=0;
    echo->sum=0;
}

/* Match the microphone to PCM accepted by the playback pipe. Keep sample
 * order across writes and leave the recent reference available for the tail. */
void db_echo_reference_rate(db_echo *echo,const unsigned char *pcm,size_t bytes,
                            uint64_t now,unsigned sample_rate) {
    unsigned decimation=sample_rate==16000?4:6;
    pthread_mutex_lock(&echo->lock);
    if(now<echo->accepted_at_ms || now-echo->accepted_at_ms>=REFERENCE_LIFETIME_MS) {
        echo->head=echo->used=0;
        echo->phase=0;
        echo->sum=0;
        echo->have_partial=0;
    }
    size_t offset=0;
    if(echo->have_partial && bytes) {
        unsigned char pair[]={echo->partial,pcm[0]};
        append(echo,sample(pair),decimation);
        echo->have_partial=0;
        offset=1;
    }
    for(;offset+1<bytes;offset+=2) append(echo,sample(pcm+offset),decimation);
    if(offset<bytes) { echo->partial=pcm[offset];echo->have_partial=1; }
    echo->accepted_at_ms=now;
    pthread_mutex_unlock(&echo->lock);
}

void db_echo_reference(db_echo *echo,const unsigned char *pcm,size_t bytes,uint64_t now) {
    db_echo_reference_rate(echo,pcm,bytes,now,24000);
}

static double correlation(db_echo *echo,const unsigned char *pcm,uint64_t now,
                          unsigned char *clean) {
    int16_t reference[DB_ECHO_REFERENCE];
    pthread_mutex_lock(&echo->lock);
    size_t used=echo->used, first=(echo->head+DB_ECHO_REFERENCE-used)%DB_ECHO_REFERENCE;
    if(used<MATCH_SAMPLES || now<echo->accepted_at_ms || now-echo->accepted_at_ms>=REFERENCE_LIFETIME_MS) {
        pthread_mutex_unlock(&echo->lock);
        return -1;
    }
    size_t contiguous=DB_ECHO_REFERENCE-first;
    if(contiguous>used) contiguous=used;
    memcpy(reference,echo->reference+first,contiguous*sizeof(*reference));
    memcpy(reference+contiguous,echo->reference,(used-contiguous)*sizeof(*reference));
    pthread_mutex_unlock(&echo->lock);

    int16_t mic[MATCH_SAMPLES];
    int64_t mic_energy=0;
    for(unsigned i=0;i<MATCH_SAMPLES;++i) {
        const unsigned char *at=pcm+i*8;
        mic[i]=(int16_t)((sample(at)+sample(at+2)+sample(at+4)+sample(at+6))/4);
        mic_energy+=(int32_t)mic[i]*mic[i];
    }
    if(mic_energy<64*MATCH_SAMPLES) return -1;
    int64_t ref_energy=0;
    double best=0;
    double gain=0;
    size_t matched_at=0;
    for(unsigned i=0;i<MATCH_SAMPLES;++i)
        ref_energy+=(int32_t)reference[i]*reference[i];
    /* A strongly correlated frame is dominated by playback. With independent
     * near-end speech in the mix, leave the microphone frame intact. */
    for(size_t first=0;first+MATCH_SAMPLES<=used;++first) {
        if(ref_energy>400*MATCH_SAMPLES) {
            int64_t product=0;
            for(unsigned i=0;i<MATCH_SAMPLES;++i)
                product+=(int32_t)reference[first+i]*mic[i];
            double squared=(double)product*product,total=(double)mic_energy*ref_energy;
            if(squared>best*total) {
                best=squared/total;
                gain=(double)product/ref_energy;
                matched_at=first;
                if(best>0.98)break;
            }
        }
        if(first+MATCH_SAMPLES==used) break;
        int old=reference[first], next=reference[first+MATCH_SAMPLES];
        ref_energy+=(int64_t)next*next-(int64_t)old*old;
    }
    if(clean && best>=0.54) {
        if(best>=0.94)memset(clean,0,DB_ECHO_FRAME_BYTES);
        else for(unsigned i=0;i<MATCH_SAMPLES;++i) {
            int predicted=(int)(gain*reference[matched_at+i]);
            for(unsigned j=0;j<4;++j) {
                size_t at=(i*4+j)*sizeof(int16_t);
                int value=sample(pcm+at)-predicted;
                if(value>INT16_MAX)value=INT16_MAX;
                if(value<INT16_MIN)value=INT16_MIN;
                int16_t result=(int16_t)value;
                memcpy(clean+at,&result,sizeof(result));
            }
        }
    }
    return best;
}

int db_echo_keyword_filter(db_echo *echo,const unsigned char *pcm,uint64_t now,
                           unsigned char *clean) {
    /* Subtract matched playback while preserving the independent remainder.
     * Neither the voice threshold nor its protection toggle applies here. */
    return correlation(echo,pcm,now,clean)>=0.54;
}

/* Keep three 20 ms frames of lead-in so confirmation does not consume the
 * start of the question. Playback/tail frames require sustained near speech;
 * listening frames follow the same queue and never pass through this gate. */
int db_echo_filter(db_echo *echo,const unsigned char *pcm,int playing,uint64_t now,
                   unsigned char *uplink) {
    unsigned bit=1u<<echo->lead_index;
    unsigned next=(echo->lead_index+1)%DB_ECHO_LEAD_FRAMES;
    unsigned char *lead=echo->lead[echo->lead_index];
    memcpy(lead,pcm,DB_ECHO_FRAME_BYTES);
    echo->echo_frames&=~bit;
    if(playing) {
        int64_t energy=0;
        for(unsigned i=0;i<DB_ECHO_FRAME_BYTES;i+=2) {
            int32_t value=sample(pcm+i);
            energy+=value*value;
        }
        /* Loud playback can still dominate the microphone. Apply echo
         * rejection at every level above the configured speech threshold. */
        const uint64_t threshold=320ull*echo->speech_rms*echo->speech_rms;
        double fit=energy>320LL*200*200?correlation(echo,pcm,now,NULL):0;
        const double echo_floor=echo->strong_protection?0.22:0.52;
        double residual=energy*(1-fit);
        /* A later near-end frame can open the speech gate. Do not replay
         * older frames that were already identified as speaker echo. */
        int reference_missing=echo->require_reference && fit<0;
        if(reference_missing || (fit>=echo_floor && (echo->require_reference || residual<=threshold)))
            echo->echo_frames|=bit;
        unsigned voiced=(uint64_t)energy>threshold && fit<echo_floor && !reference_missing;
        unsigned recent=echo->strong_protection?255u:7u;
        unsigned votes=echo->strong_protection?4u:2u;
        echo->near_votes=((echo->near_votes<<1)|voiced)&recent;
        if(voiced && (unsigned)__builtin_popcount(echo->near_votes)>=votes)
            echo->near_until_ms=now+SPEECH_HOLD_MS;
        echo->protected_frames|=bit;
    } else {
        echo->near_votes=0;
        echo->protected_frames&=~bit;
    }
    echo->lead_index=next;
    if(echo->lead_count<DB_ECHO_LEAD_FRAMES) ++echo->lead_count;
    int suppress=echo->lead_count<DB_ECHO_LEAD_FRAMES ||
        (echo->echo_frames&(1u<<next)) ||
        ((echo->protected_frames&(1u<<next)) && now>=echo->near_until_ms);
    if(suppress) memset(uplink,0,DB_ECHO_FRAME_BYTES);
    else memcpy(uplink,echo->lead[next],DB_ECHO_FRAME_BYTES);
    return suppress && playing;
}
