/* Enrollment calibration shares the production MFCC and duration-bank search.
 * The calibration identity binds both features and live detection semantics. */
#define WW_MFCC 3
#define main wakeword_template_cli_main
#include "wakeword_template.c"
#undef main

/* Each duration must produce its own three consecutive confirmations. Track
 * the best confirmed production duration independently for each start phase. */
static void stream_duration(int window, const int16_t *pcm, int samples,
                            Model *scorer, float best[4]) {
    float trails[4][3];
    for(int p=0;p<4;p++)for(int k=0;k<3;k++)trails[p][k]=99;
    const int padding=SAMPLE_RATE/2;
    int16_t segment_pcm[MAX_UTT_SAMPLES];
    for(int end=BLOCK_SAMPLES;end<=padding+samples+window;end+=BLOCK_SAMPLES) {
        if(end<window)continue;
        int block=end/BLOCK_SAMPLES;
        int phase=block%4,slot=(block/4)%3;
        for(int j=0;j<window;j++) {
            int pos=end-window+j-padding;
            segment_pcm[j]=pos>=0&&pos<samples?pcm[pos]:0;
        }
        mel_cache_origin=(uint64_t)(end-window);
        trails[phase][slot]=live_window_score(scorer,segment_pcm,window);
        float stable=fmaxf(trails[phase][0],fmaxf(trails[phase][1],trails[phase][2]));
        if(stable<best[phase])best[phase]=stable;
    }
}

static float stream_required(Model *model, const int16_t *pcm, int samples, Model *scorer) {
    int window[WW_MAX_WINDOWS], n=model_windows(model,window), nearest=0;
    for(int w=1;w<n;w++)
        if(abs(window[w]-samples)<abs(window[nearest]-samples))nearest=w;
    float best[4]={99,99,99,99};
    float limit=scorer->has_threshold?scorer->threshold:WAKE_AUTO_GATE_CEILING;
    if(!scorer->has_threshold && model==scorer)limit=(limit-.01f)/1.10f;
    /* Preserve the nearest-duration result when it certifies the gate.
     * When it cannot, try the other durations available to the detector. */
    for(int pass=0;pass<n;pass++) {
        int w=pass==0?nearest:(pass<=nearest?pass-1:pass);
        stream_duration(window[w],pcm,samples,scorer,best);
        float worst=fmaxf(fmaxf(best[0],best[1]),fmaxf(best[2],best[3]));
        if(worst<=limit || pass+1==n)return worst;
    }
    return 99;
}

/* For negatives any phase/duration that confirms is a potential cross hit. */
static float stream_cross_min(Model *model, const int16_t *pcm, int samples) {
    int window[WW_MAX_WINDOWS], nw = model_windows(model, window);
    float trails[4][WW_MAX_WINDOWS][3], best = 99;
    int counts[4] = {0}; int16_t segment_pcm[MAX_UTT_SAMPLES];
    for (int p=0;p<4;p++) for (int w=0;w<nw;w++) for (int k=0;k<3;k++) trails[p][w][k]=99;
    int padding = SAMPLE_RATE;
    for (int end=BLOCK_SAMPLES; end <= padding+samples+window[nw-1]; end+=BLOCK_SAMPLES) {
        int phase=(end/BLOCK_SAMPLES)%4, slot=counts[phase]++%3;
        for (int w=0;w<nw;w++) {
            float score=99;
            if (end>=window[w]) {
                for(int j=0;j<window[w];j++) {int pos=end-window[w]+j-padding;
                    segment_pcm[j]=pos>=0&&pos<samples?pcm[pos]:0;}
                mel_cache_origin=(uint64_t)(end-window[w]);
                score=live_window_score(model,segment_pcm,window[w]);
            }
            trails[phase][w][slot]=score;
            float stable=fmaxf(trails[phase][w][0],fmaxf(trails[phase][w][1],trails[phase][w][2]));
            if(stable<best)best=stable;
        }
    }
    return best;
}
#include "wakeword_joint.inc"

int main(int argc,char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "joint")) return joint_main(argc, argv);
    int cross_at=-1;
    for(int i=2;i<argc;i++) if(!strcmp(argv[i],"--cross")) cross_at=i;
    int own_end=cross_at<0?argc:cross_at;
    int count=own_end-2;
    if(count<3 || count>MAX_TEMPLATES || (cross_at>=0 && cross_at+1>=argc)) {
        fprintf(stderr,"usage: calibrate MODEL REC1 REC2 REC3 ... [--cross OTHER.wav ...]\n");
        return 2;
    }
    init_features();
    Model model={0};
    char modelarg[512];
    if(snprintf(modelarg,sizeof(modelarg),"calibrate=%s",argv[1])>=(int)sizeof(modelarg))return 2;
    if(load_model(&model,modelarg))return 2;
    if(model.count!=(uint32_t)count)return 2;
    Template records[MAX_TEMPLATES]={{0}};
    int16_t *audio[MAX_TEMPLATES]={0};int lengths[MAX_TEMPLATES]={0};
    for(int i=0;i<count;i++) {
        if(read_wav(argv[i+2],&audio[i],&lengths[i]) || lengths[i]<MIN_UTT_SAMPLES || lengths[i]>MAX_UTT_SAMPLES)return 2;
        records[i].feat=extract_features(audio[i],lengths[i],&records[i].frames);
        if(!records[i].feat)return 2;
    }
    float worst_stream=0,worst_cross=0,worst_held=0;
    float heldout[MAX_TEMPLATES];
    int stream_record=0,cross_record=0,held_record=0;
    for(int i=0;i<count;i++) {
        /* Reuse identical interior frames across alignments and the full/held
         * scoring passes, but never across distinct enrollment recordings. */
#ifndef WW_DISABLE_MEL_CACHE
        mel_cache_enabled=1;
#endif
        memset(mel_cache_tags,0,sizeof(mel_cache_tags));
        Model other={0};
        for(int j=0;j<count;j++)if(i!=j)other.items[other.count++]=records[j];
        float cross=score_model(&other,records[i].feat,(int)records[i].frames);
        float live=stream_required(&model,audio[i],lengths[i],&model);
        /* Keep production windows, but exclude this recording from scoring.
         * A self-match otherwise contributes 65% of the nearest-template score
         * and can certify a gate that rejects the next real utterance. */
        heldout[i]=stream_required(&model,audio[i],lengths[i],&other);
        fprintf(stderr,"HOLDOUT rec=%d stream=%.6f\n",i+1,heldout[i]);
        fprintf(stderr,"CALIBRATION rec=%d cross=%.6f stream=%.6f\n",i+1,cross,live);
        if(!isfinite(cross) || cross>worst_cross){worst_cross=cross;cross_record=i+1;}
        if(!isfinite(live) || live>worst_stream){worst_stream=live;stream_record=i+1;}
    }
    /* Keep both the consistency ceiling and the independent automatic gate.
     * The joint pass requires every held-out recording, so its maximum must
     * also be a lower bound when deriving a new model's initial gate. */
    if(!(worst_cross<=.35f)) {
        fprintf(stderr,"CALIBRATION_REJECT rec=%d reason=consistency required=%.6f limit=0.35\n",cross_record,worst_cross);
        return 3;
    }
    for(int i=0;i<count;i++) {
        if(!isfinite(heldout[i]) || heldout[i]>=99) {
            fprintf(stderr,"CALIBRATION_REJECT rec=%d reason=stream required=99 limit=%.2f\n",i+1,WAKE_AUTO_GATE_CEILING);
            return 3;
        }
        if(heldout[i]>worst_held) {worst_held=heldout[i];held_record=i+1;}
    }
    float threshold=fmaxf(.02f,worst_stream*1.10f+.01f);
    for(int i=0;i<count;i++)for(int j=i+1;j<count;j++)
        if(heldout[j]<heldout[i]){float x=heldout[i];heldout[i]=heldout[j];heldout[j]=x;}
    float median=count%2?heldout[count/2]:(heldout[count/2-1]+heldout[count/2])*.5f;
    /* Keep the recording-specific gate and enforce the cross-word check
     * before the candidate becomes active. */
    threshold=fmaxf(threshold,fminf(WAKE_AUTO_GATE_CEILING,fminf(threshold+.08f,median*1.05f+.01f)));
    if(!isfinite(worst_stream) || !(threshold<=WAKE_AUTO_GATE_CEILING)) {
        fprintf(stderr,"CALIBRATION_REJECT rec=%d reason=stream required=%.6f limit=%.2f\n",stream_record,threshold,WAKE_AUTO_GATE_CEILING);
        return 3;
    }
    if (!(worst_held <= WAKE_AUTO_GATE_CEILING)) {
        fprintf(stderr,"CALIBRATION_REJECT rec=%d reason=holdout required=%.6f limit=%.2f\n",held_record,worst_held,WAKE_AUTO_GATE_CEILING);
        return 3;
    }
    /* Round up the minimum requirement before the joint check reads the
     * six-decimal on-disk gate. The ceiling and every cross check still apply. */
    double required = ceil((double)worst_held * 1000000.0) / 1000000.0;
    threshold = fmaxf(threshold, (float)required);
    /* Recheck quiet enrollment with the actual strict quiet-input policy.
     * Normal and quiet recordings share the same MFCC and per-word gate;
     * missing detail is never compensated by widening a quiet-word threshold. */
    model.threshold=threshold;model.has_threshold=1;
    if(cross_at>=0) {
        float cross_scores[64]; int ncross=0, near=0, conflict;
        float tightened=threshold;
        for(int i=cross_at+1;i<argc && ncross<64;i++) {
            int16_t *pcm=NULL; int samples=0;
            if(read_wav(argv[i],&pcm,&samples) || samples<MIN_UTT_SAMPLES || samples>MAX_UTT_SAMPLES) {
                fprintf(stderr,"CALIBRATION_REJECT reason=cross-read file=%s\n",argv[i]);
                return 2;
            }
            memset(mel_cache_tags,0,sizeof(mel_cache_tags));
            cross_scores[ncross]=stream_cross_min(&model,pcm,samples);
            fprintf(stderr,"CROSS file=%s score=%.6f\n",argv[i],cross_scores[ncross]);
            free(pcm);
            ncross++;
        }
        conflict=wake_joint_tighten(threshold,WAKE_AUTO_GATE_CEILING,WAKE_NORM_BAND,heldout,count,cross_scores,ncross,&tightened,&near);
        if(conflict) {
            fprintf(stderr,"CALIBRATION_CONFLICT reason=cross threshold=%.6f near=%d\n",threshold,near);
            return 4;
        }
        if(tightened<threshold) {
            fprintf(stderr,"CROSS_TIGHTEN from=%.6f to=%.6f near=%d\n",threshold,tightened,near);
            threshold=tightened; model.threshold=threshold;
        } else if(near) fprintf(stderr,"CROSS_NEAR near=%d threshold=%.6f\n",near,threshold);
    }
    for(int i=0;i<count;i++) {
        double power=0;for(int j=0;j<lengths[i];j++)power+=(double)audio[i][j]*audio[i][j];
        if(power/lengths[i]>WW_NORMAL_RMS*WW_NORMAL_RMS)continue;
        memset(mel_cache_tags,0,sizeof(mel_cache_tags));
        if(stream_required(&model,audio[i],lengths[i],&model)>threshold) {
            fprintf(stderr,"CALIBRATION_REJECT rec=%d reason=quiet required=99 limit=%.2f\n",i+1,WAKE_AUTO_GATE_CEILING);
            return 3;
        }
    }
    printf("CALIBRATED %.6f %s:%d:%s recordings=%d\n",threshold,FEATURE_NAME,MODEL_VERSION,WW_CALIBRATION_POLICY,count);
    return 0;
}
