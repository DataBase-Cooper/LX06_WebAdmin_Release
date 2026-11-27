/* Short templates are derived from the active model's enrollment recordings. */
#define PHRASE_TAIL_BLOCKS 6
#define PHRASE_MAX_SCORE .225f
#define PHRASE_RIVAL_MARGIN .065f

struct wake_phrase {
    Model models[MAX_MODELS];
    float levels[MAX_UTT_SAMPLES/BLOCK_SAMPLES];
    int active, floor, last_end, attempts;
};

static int phrase_order(const void *a, const void *b) {
    float x=*(const float *)a, y=*(const float *)b;
    return (x>y)-(x<y);
}

static int phrase_crop(const int16_t *pcm, int samples, int *offset) {
    float levels[MAX_UTT_SAMPLES/BLOCK_SAMPLES], sorted[MAX_UTT_SAMPLES/BLOCK_SAMPLES];
    int count=samples/BLOCK_SAMPLES;
    if (count<25 || count>(int)(sizeof levels/sizeof *levels)) return 0;
    for (int i=0;i<count;++i) levels[i]=block_rms(pcm+i*BLOCK_SAMPLES);
    memcpy(sorted,levels,(size_t)count*sizeof *sorted);
    qsort(sorted,(size_t)count,sizeof *sorted,phrase_order);
    float limit=fmaxf(100.f,fmaxf(2.5f*sorted[(count-1)/5],.15f*sorted[(count-1)*95/100]));
    int first=-1,last=-1,voiced=0;
    for (int i=0;i<count;++i) {
        int votes=(levels[i]>=limit)+(i>0 && levels[i-1]>=limit)+
            (i+1<count && levels[i+1]>=limit);
        if (votes<2) continue;
        if (first<0) first=i;
        last=i;
        ++voiced;
    }
    if (voiced<12 || last-first<24) return 0;
    first=first>5 ? first-5 : 0;
    int end=last+1+PHRASE_TAIL_BLOCKS;
    if (end>count) end=count;
    *offset=first*BLOCK_SAMPLES;
    return (end-first)*BLOCK_SAMPLES;
}

static void phrase_free(struct wake_phrase *fast, int count) {
    for (int i=0;i<count;++i)
        for (unsigned j=0;j<fast->models[i].count;++j)
            free(fast->models[i].items[j].feat);
}

static int phrase_load_word(const Model *model, Model *short_model) {
    if (model->count<3 || model->count>5 || strchr(model->label,'/') ||
        strchr(model->label,'\\') || !strcmp(model->label,".") ||
        !strcmp(model->label,"..")) return 0;
    for (int slot=1;slot<=10;++slot) {
        char path[512];
        int n=snprintf(path,sizeof path,"%s/recs/%s/%d.wav",p05_data_root(),model->label,slot);
        if (n<0 || n>=(int)sizeof path) return 0;
        struct stat st;
        if (lstat(path,&st)) {
            if (errno==ENOENT) continue;
            return 0;
        }
        if (!S_ISREG(st.st_mode) || short_model->count==model->count) return 0;
        int16_t *pcm=NULL;
        int samples,offset=0;
        if (read_wav(path,&pcm,&samples)) return 0;
        uint32_t frames=0;
        const Template *original=&model->items[short_model->count];
        /* Stale or replaced recordings must never authorize a short match. */
        int current=enrollment_template_samples(pcm,samples,original);
        int width=current ? phrase_crop(pcm,current,&offset) : 0;
        float *short_features=width ? extract_features(pcm+offset,width,&frames) : NULL;
        free(pcm);
        if (!short_features) return 0;
        short_model->items[short_model->count++]=(Template){frames,short_features};
    }
    return short_model->count==model->count;
}

static void phrase_prepare(struct wake_phrase *fast, const Model *models, int count) {
    if (!paired || p05_raw_policy || count<1) return;
    for (int i=0;i<count;++i) {
        if (phrase_load_word(&models[i],&fast->models[i])) continue;
        phrase_free(fast,count);
        memset(fast->models,0,sizeof fast->models);
        return;
    }
    fast->active=1;
    fprintf(stderr,"PHRASE_READY words=%d\n",count);
}

static int phrase_fast_decide(struct wake_phrase *fast, Model *models, int count,
                              const int16_t *pcm, int used, int blocks, float fallback,
                              int *cooldown, int *used_io) {
    if (!fast->active) return 0;
    const int capacity=MAX_UTT_SAMPLES/BLOCK_SAMPLES;
    fast->levels[(blocks-1)%capacity]=block_rms(pcm+used-BLOCK_SAMPLES);
    if (*cooldown) {
        fast->floor=blocks;
        return 0;
    }
    int n=used/BLOCK_SAMPLES;
    if (n<33 || blocks-fast->floor<33) return 0;
    if (n>blocks-fast->floor) n=blocks-fast->floor;
    float recent[MAX_UTT_SAMPLES/BLOCK_SAMPLES], sorted[MAX_UTT_SAMPLES/BLOCK_SAMPLES];
    for (int i=0;i<n;++i) recent[i]=fast->levels[(blocks-n+i)%capacity];
    memcpy(sorted,recent,(size_t)n*sizeof(*sorted));
    qsort(sorted,(size_t)n,sizeof(*sorted),phrase_order);
    float limit=fmaxf(100.f,fmaxf(2.5f*sorted[(n-1)/5],.15f*sorted[(n-1)*95/100]));
    for (int i=n-PHRASE_TAIL_BLOCKS;i<n;++i)
        if (recent[i]>=limit) return 0;
    int first=-1,last=-1,voiced=0;
    for (int i=0;i<n;++i) {
        int votes=(recent[i]>=limit)+(i && recent[i-1]>=limit)+(i+1<n && recent[i+1]>=limit);
        if (votes<2) continue;
        if (first<0) first=i;
        last=i;
        ++voiced;
    }
    if (voiced<12 || last-first<24 || n-last>12) return 0;
    int end=blocks-n+last+1+PHRASE_TAIL_BLOCKS;
    if (end>blocks) return 0;
    if (end!=fast->last_end) {
        fast->last_end=end;
        fast->attempts=0;
    }
    if (fast->attempts++>=2) return 0;
    if (first>=5) first-=5;
    else first=0;
    const int16_t *source=pcm+used-(n-first)*BLOCK_SAMPLES;
    int width=(last+1+PHRASE_TAIL_BLOCKS-first)*BLOCK_SAMPLES;
    if (width<9600 || width>40000) return 0;
    double power=0;
    for (int i=0;i<width;++i) power+=(double)source[i]*source[i];
    if (power/width < WW_QUIET_RMS*WW_QUIET_RMS) return 0;
    float trail[MAX_MODELS][3], scores[MAX_MODELS], rms[MAX_MODELS], gates[MAX_MODELS];
    int lengths[MAX_MODELS], ends[MAX_MODELS], winner=-1;
    for (int i=0;i<count;++i) scores[i]=0.f;
    for (int step=0;step<3;++step) {
        uint32_t frames;
        mel_cache_origin=(uint64_t)(blocks-n+first)*BLOCK_SAMPLES;
        float *features=extract_features(source,width-(2-step)*BLOCK_SAMPLES,&frames);
        if (!features) return 0;
        for (int i=0;i<count;++i) {
            trail[i][step]=score_model(&fast->models[i],features,(int)frames);
            scores[i]=fmaxf(scores[i],trail[i][step]);
        }
        free(features);
    }
    for (int i=0;i<count;++i) {
        lengths[i]=width;
        ends[i]=20*end;
        rms[i]=(float)sqrt(power/width);
        gates[i]=models[i].has_threshold ? models[i].threshold : fallback;
        if (scores[i]<=fminf(gates[i],PHRASE_MAX_SCORE)) {
            if (winner>=0) return 0;
            winner=i;
        }
    }
    if (winner<0) return 0;
    /* When cloud verification is enabled, leave borderline candidates to the
     * ordinary matcher so it can submit its complete verification clip. */
    if (p05_cloud_on() && gates[winner]>WAKE_AUTO_GATE_CEILING) return 0;
    for (int i=0;i<count;++i) if (i!=winner) {
        for (int j=0;j<3;++j) {
            if (gates[i]<=0 || trail[i][j]-scores[winner]<PHRASE_RIVAL_MARGIN ||
                trail[i][j]/gates[i]<=1.f+WAKE_NORM_BAND ||
                trail[i][j]/gates[i]-scores[winner]/gates[winner]<WAKE_GRAY_MARGIN) return 0;
        }
    }
    struct pair_ticket ticket;
    if (!pair_prepare(&ticket,source,width,ends[winner])) return 0;
    ticket.phrase_fast=1;
    p05_decide_with_pair(models,count,scores,rms,lengths,trail,ends,NULL,gates,
                         used,blocks,cooldown,used_io,&ticket);
    if (!*cooldown) return 0;
    fast->floor=blocks;
    fprintf(stderr,"PHRASE_WAKE %s span_ms=%d tail_ms=%d capture_ms=%llu dispatch_ms=%llu\n",
            models[winner].label,width/16,PHRASE_TAIL_BLOCKS*20,
            (unsigned long long)ticket.stamp,(unsigned long long)pair_now_ms());
    return 1;
}
