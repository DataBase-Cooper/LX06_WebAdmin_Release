/* T07/T08. Same MFCC/scorer/duration/confirmation functions, no new matcher.
 * Caller owns the shared kernel mutation lock through the complete operation.
 * Input roots are enrollment recs plus explicitly opted-in tmpfs diagnostics;
 * acceptance/test directories are deliberately never enumerated. */
#include "ww_store.c"

#define JOINT_WORDS 6
struct joint_word {
    char name[48], sha[65]; Model model;
    float base, next, candidate_limit, held[MAX_TEMPLATES], cross[80], cells[JOINT_WORDS+1];
    int nheld, ncross, enabled, conflict, near, calibrated, pending;
    int held_record[MAX_TEMPLATES], cross_hit;
    unsigned retry_mask;
};
static int joint_safe_name(const char *name) {
    size_t n = strlen(name);
    if (n < 6 || n > 18 || n%3) return 0;
    for (size_t i=0;i<n;i+=3) {
        unsigned a=(unsigned char)name[i], b=(unsigned char)name[i+1], c=(unsigned char)name[i+2];
        unsigned cp=((a&15)<<12)|((b&63)<<6)|(c&63);
        if ((a&0xf0)!=0xe0 || (b&0xc0)!=0x80 || (c&0xc0)!=0x80 || cp<0x4e00 || cp>0x9fff) return 0;
    }
    return 1;
}
static int joint_decimal(const char *text) {
    int digits=0, dots=0;
    if(!text || !*text)return 0;
    for(const char *p=text;*p;p++) {
        if(*p>='0' && *p<='9')digits++;
        else if(*p=='.')dots++;
        else return 0;
    }
    return digits>0 && dots<=1;
}
static int joint_field(const char *line, const char *key, char *out, size_t cap) {
    size_t len=strlen(key); out[0]=0;
    while (*line) {
        const char *end=strchr(line,'|'); size_t n=end?(size_t)(end-line):strlen(line);
        if (n>len && !memcmp(line,key,len) && line[len]=='=') {
            n-=len+1; if (n>=cap) return -1;
            memcpy(out,line+len+1,n);out[n]=0;return 0;
        }
        if (!end) break;
        line=end+1;
    }
    return -1;
}
/* Every update replaces the full word set in a single conf publication. Keep
 * routing, unknown fields, comments and official options byte-for-byte. */
static int joint_conf(const char *data, const char *original, struct joint_word *w, int nw, int after) {
    char out[32768]; size_t used=0; const char *p=original;
    while (*p) {
        const char *e=strchr(p,'\n'); size_t len=e?(size_t)(e-p):strlen(p);
        char line[1024], name[48]; if(len>=sizeof(line))return -1;
        memcpy(line,p,len);line[len]=0;
        int wi=-1;
        if (!strncmp(line,"word=",5) && !joint_field(line,"word",name,sizeof(name)))
            for(int i=0;i<nw;i++)if(!strcmp(name,w[i].name))wi=i;
        if(wi<0) {if(used+len+2>=sizeof(out))return -1;memcpy(out+used,p,len);used+=len;out[used++]='\n';}
        else {
            struct joint_word *word=&w[wi]; char *field=line;
            double accepted=ww_last_accepted_gate(p,len,NULL);
            int certified=after && word->calibrated && !word->conflict;
            if(certified && (!accepted || word->next<accepted || word->candidate_limit>0))
                accepted=word->next;
            while (*field) {
                char *end=strchr(field,'|'); if(end)*end=0;
                if(strncmp(field,"threshold=",10) && strncmp(field,"enabled=",8) &&
                   strncmp(field,"p05_conflict=",13) && strncmp(field,"p05_pending=",12) &&
                   strncmp(field,"p05_requested=",14) && strncmp(field,"p05_near=",9) &&
                   (!certified || strncmp(field,"p05_accepted_sha=",17)) &&
                   (!after || strncmp(field,"p05_candidate=",14)) &&
                   strncmp(field,WW_ACCEPTED_GATE_FIELD,sizeof(WW_ACCEPTED_GATE_FIELD)-1)) {
                    size_t n=strlen(field);if(used+n+2>=sizeof(out))return -1;
                    memcpy(out+used,field,n);used+=n;out[used++]='|';
                }
                if(!end)break;
                field=end+1;
            }
            if(accepted>0) {
                int n=snprintf(out+used,sizeof(out)-used,WW_ACCEPTED_GATE_FIELD "%.6f|",accepted);
                if(n<0 || (size_t)n>=sizeof(out)-used)return -1;
                used+=(size_t)n;
            }
            int pending=!after && word->pending;
            int n=snprintf(out+used,sizeof(out)-used,"threshold=%.6f|enabled=%d|p05_conflict=%d|p05_near=%d",
                after?word->next:word->base,word->enabled&&!word->conflict&&!pending,word->conflict,word->near);
            if(n<0 || (size_t)n>=sizeof(out)-used)return -1;
            used+=(size_t)n;
            if(pending) {
                n=snprintf(out+used,sizeof(out)-used,"|p05_pending=1|p05_requested=%d",word->enabled);
                if(n<0 || (size_t)n>=sizeof(out)-used)return -1;
                used+=(size_t)n;
            }
            if(used+1>=sizeof(out))return -1;
            out[used++]='\n';
        }
        p=e?e+1:p+len;
    }
    return ww_store_replace_conf(data,out,used);
}
static int joint_report(const char *path, struct joint_word *w, int nw, int after) {
    FILE *f=fopen(path,"wx"); if(!f)return -1;
    fprintf(f,"{\"version\":1,\"purpose\":\"train+diagnostic\",\"policy\":\"%s\",\"phase\":\"%s\",\"words\":[",WW_CALIBRATION_POLICY,after?"calibrated":"original");
    for(int i=0;i<nw;i++) {
        fprintf(f,"%s{\"name\":\"%s\",\"model_sha256\":\"%s\",\"threshold\":%.6f,\"next_threshold\":%.6f,\"conflict\":%s,\"near\":%s,\"calibrated\":%s}",
            i?",":"",w[i].name,w[i].sha,w[i].base,after?w[i].next:w[i].base,
            w[i].conflict?"true":"false",w[i].near?"true":"false",after&&w[i].calibrated?"true":"false");
    }
    fprintf(f,"],\"cells\":[");int first=1;
    for(int i=0;i<nw;i++)for(int j=0;j<=nw;j++) {
        if(j==nw && w[i].cells[j]>=99)continue;
        float score=w[i].cells[j];
        if(i==j){score=0;for(int h=0;h<w[i].nheld;h++)if(w[i].held[h]>score)score=w[i].held[h];}
        fprintf(f,"%s{\"model\":\"%s\",\"actual\":\"%s\",\"score\":%.6f,\"c\":%.6f}",first?"":",",w[i].name,j==nw?"不是唤醒词":w[j].name,score,score/(after?w[i].next:w[i].base));first=0;
    }
    fprintf(f,"],\"acceptance\":\"pending device\"}\n");
    int rc=fflush(f) || fsync(fileno(f));if(fclose(f))rc=1;return rc?-1:0;
}
static int joint_audio(struct joint_word *word, const char *path, int actual, int own) {
    int16_t *pcm=NULL;int samples=0;
    if(read_wav_bounded(path,&pcm,&samples,0) || samples<MIN_UTT_SAMPLES || samples>80000)return -1;
#ifndef WW_DISABLE_MEL_CACHE
    mel_cache_enabled=1;
#endif
    memset(mel_cache_tags,0,sizeof(mel_cache_tags));
    if(own) {
        mel_cache_enabled=0;
        if(word->nheld>=MAX_TEMPLATES || word->nheld>=(int)word->model.count ||
           !enrollment_template_samples(pcm,samples,&word->model.items[word->nheld])) {
            free(pcm);
            return -1;
        }
#ifndef WW_DISABLE_MEL_CACHE
        mel_cache_enabled=1;
#endif
        memset(mel_cache_tags,0,sizeof(mel_cache_tags));
        /* The template order is the numeric recording order in ww-train. */
        Model scorer=word->model;scorer.count=0;
        for(uint32_t j=0;j<word->model.count;j++)if(j!=(unsigned)word->nheld)scorer.items[scorer.count++]=word->model.items[j];
        word->held[word->nheld++]=stream_required(&word->model,pcm,samples,&scorer);
    } else {
        float score=stream_cross_min(&word->model,pcm,samples);
        if(!isfinite(score) || word->ncross>=80){free(pcm);return -1;}
        word->cross[word->ncross++]=score;
        if(score<word->cells[actual])word->cells[actual]=score;
    }
    free(pcm);return 0;
}
static void joint_prune_reports(const char *root, const char *current, const char *previous) {
    DIR *d=opendir(root);struct dirent *e;
    while(d && (e=readdir(d))) {
        if(strncmp(e->d_name,"run-",4) || strlen(e->d_name)!=10 || !strcmp(e->d_name,current) || !strcmp(e->d_name,previous))continue;
        int child=openat(dirfd(d),e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(child<0)continue;
        unlinkat(child,"original.json",0);unlinkat(child,"calibrated.json",0);unlinkat(child,"status",0);close(child);
        unlinkat(dirfd(d),e->d_name,AT_REMOVEDIR);
    }
    if(d)closedir(d);
}
static int joint_main(int argc,char **argv) {
    /* joint DATA REPORT_ROOT [THRESHOLD_WORD THRESHOLD]. The web worker and
     * ww-train inherit fd9. CLI callers get the same nonblocking lock. */
    if(argc!=4 && argc!=6)return 2;
    int mutation=-1;
    if(!getenv("LX06_WW_LOCK_HELD") || strcmp(getenv("LX06_WW_LOCK_HELD"),"1")) {
        mutation=open("/tmp/lx06-ww-mutate.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        if(mutation<0 || flock(mutation,LOCK_EX|LOCK_NB)) {if(mutation>=0)close(mutation);return 2;}
    } else if(fcntl(9,F_GETFD)<0 || flock(9,LOCK_EX|LOCK_NB)) return 2;
    int rc=2,nw=0;char confpath[512],*conf=NULL;size_t cn=0;
    struct joint_word *words=calloc(JOINT_WORDS,sizeof(*words));
    if(!words)goto done;
    snprintf(confpath,sizeof(confpath),"%s/wakeword.conf",argv[2]);
    int fd=open(confpath,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)goto done;
    int readrc=read_fd(fd,&conf,&cn);close(fd);if(readrc || cn>16000)goto done;
    init_features();
    char *copy=strdup(conf),*save=NULL,*line=copy?strtok_r(copy,"\n",&save):NULL;
    for(;line;line=strtok_r(NULL,"\n",&save)) {
        if(strncmp(line,"word=",5))continue;
        char name[48],gate[32],feature[80],sha[65],enabled[8],path[512],arg[600];
        if(joint_field(line,"word",name,sizeof(name)) || !joint_safe_name(name) || nw>=JOINT_WORDS){free(copy);goto done;}
        snprintf(path,sizeof(path),"%s/models/%s.lwt",argv[2],name);
        if(access(path,F_OK))continue; /* newly added word with no model */
        char pending[8],requested[8];
        joint_field(line,"p05_pending",pending,sizeof(pending));
        int no_hash=joint_field(line,"model_sha256",sha,sizeof(sha)) || !sha[0];
        /* An enrollment that changed samples and did not produce a new
         * candidate cannot characterize other words using its stale model. */
        if(pending[0]=='1' && no_hash)continue;
        if(joint_field(line,"threshold",gate,sizeof(gate)) || joint_field(line,"feature",feature,sizeof(feature)) ||
           strcmp(feature,"mfcc-lifter32:8:" WW_CALIBRATION_POLICY) || no_hash) {free(copy);goto done;}
        struct joint_word *w=&words[nw];strcpy(w->name,name);
        char *end=NULL;if(!joint_decimal(gate)){free(copy);goto done;}w->base=strtof(gate,&end);if(!end || *end || !isfinite(w->base) || w->base<=0 || w->base>.99f){free(copy);goto done;}
        if(argc==6 && !strcmp(name,argv[4])) {if(!joint_decimal(argv[5])){free(copy);goto done;}w->base=strtof(argv[5],&end);if(!end || *end || !isfinite(w->base) || w->base<=0 || w->base>WAKE_GATE_CEILING){free(copy);goto done;}}
        w->next=w->base;joint_field(line,"enabled",enabled,sizeof(enabled));w->enabled=enabled[0]!='0';
        if(pending[0]=='1'){joint_field(line,"p05_requested",requested,sizeof(requested));w->enabled=requested[0]=='1';}
        w->pending=pending[0]=='1' || (argc==6 && !strcmp(name,argv[4]));
        char prior[8];joint_field(line,"p05_conflict",prior,sizeof(prior));/* A newly recorded candidate is judged from the current matrix.
         * Preserve existing flags for other retained words. */
        w->conflict=pending[0]!='1' && prior[0]=='1';
        if(wake_sha256_file(path,w->sha) || strcmp(w->sha,sha)) {free(copy);goto done;}
        snprintf(arg,sizeof(arg),"%s=%s:%.9g",name,path,w->base);
        if(load_model(&w->model,arg) || w->model.count<WAKE_ENROLL_MIN || w->model.count>WAKE_ENROLL_LEGACY_MAX){free(copy);goto done;}
        char old_sha[65], proposed[32];
        if(argc==4 && pending[0]=='1' &&
           !joint_field(line,"p05_accepted_sha",old_sha,sizeof(old_sha)) &&
           strlen(old_sha)==64 && strspn(old_sha,"0123456789abcdef")==64 && strcmp(old_sha,sha) &&
           !joint_field(line,"p05_candidate",proposed,sizeof(proposed))) {
            double limit=ww_gate_value(proposed,strlen(proposed));
            if(limit>0 && limit<=WAKE_AUTO_GATE_CEILING)w->candidate_limit=(float)limit;
        }
        for(int j=0;j<=JOINT_WORDS;j++)w->cells[j]=99;
        nw++;
    }
    free(copy);if(!nw)goto done;
    if(argc==6){int found=0;for(int i=0;i<nw;i++)if(!strcmp(words[i].name,argv[4]))found=1;if(!found)goto done;}
    /* Try the previous model's strict gate first. Only an independently
     * calibrated replacement may request the minimum extra room required
     * by its own held-outs. Cross-word and diagnostic samples below are
     * always scored using the resulting gate, including quiet-input rules. */
    for(int i=0;i<nw;i++) {
        struct joint_word *w=&words[i];
        if(!w->candidate_limit)continue;
        float needed=w->base;
        for(int r=1;r<=WAKE_ENROLL_LEGACY_MAX;r++) {
            char rec[512];snprintf(rec,sizeof(rec),"%s/recs/%s/%d.wav",argv[2],w->name,r);
            if(access(rec,F_OK))continue;
            if(joint_audio(w,rec,i,1))goto done;
            needed=fmaxf(needed,w->held[w->nheld-1]);
        }
        if(needed>w->base) {
            float rounded=(float)(ceil((double)needed*1000000.0)/1000000.0);
            w->base=fminf(w->candidate_limit,rounded);
            w->model.threshold=w->next=w->base;
        }
        w->nheld=0;
    }
    for(int i=0;i<nw;i++)for(int j=0;j<nw;j++) {
        int nrec=0;
        for(int r=1;r<=WAKE_ENROLL_LEGACY_MAX;r++) {
            char path[512];snprintf(path,sizeof(path),"%s/recs/%s/%d.wav",argv[2],words[j].name,r);
            if(access(path,F_OK))continue;
            if(joint_audio(&words[i],path,j,i==j))goto done;
            if(i==j)words[i].held_record[nrec]=r;
            nrec++;
        }
        if(nrec<WAKE_ENROLL_MIN || (i==j && nrec!=(int)words[i].model.count))goto done;
    }
    /* Explicit diagnostics remain in the existing capped tmpfs forensics ring.
     * No copies in DATA, no acceptance samples used for tuning. */
    const char *diag=getenv("LX06_WAKE_FORENSICS_DIR");if(!diag)diag=WAKE_FORENSICS_DIR;
    DIR *d=opendir(diag);struct dirent *entry;
    while(d && (entry=readdir(d))) {
        size_t len=strlen(entry->d_name);char raw[8192],actual[64],path[512];
        if(len<6 || len>53 || strcmp(entry->d_name+len-5,".json"))continue;
        int in=openat(dirfd(d),entry->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(in<0)continue;
        ssize_t n=read(in,raw,sizeof(raw)-1);close(in);if(n<=0)continue;raw[n]=0;
        char *label=strstr(raw,"\"label\":");
        if(!label || !strstr(label,"\"cross_sample\":true"))continue;
        if(json_get(label,"should_be",actual,sizeof(actual))){closedir(d);goto done;}
        int j=nw;for(int k=0;k<nw;k++)if(!strcmp(actual,words[k].name))j=k;
        if(j==nw && strcmp(actual,"不是唤醒词")){closedir(d);goto done;}
        snprintf(path,sizeof(path),"%s/%.*s.wav",diag,(int)(len-5),entry->d_name);
        for(int i=0;i<nw;i++)if(i!=j && joint_audio(&words[i],path,j,0)){closedir(d);goto done;}
    }
    if(d)closedir(d);
    for(int i=0;i<nw;i++)for(int j=0;j<=nw;j++)if(i!=j) {
        float c=words[i].cells[j]/words[i].base;
        if(c<1) {
            words[i].conflict=words[i].cross_hit=1;
            if(j<nw)words[j].conflict=words[j].cross_hit=1;
        }
        else if(c<1+WAKE_NORM_BAND){words[i].near=1;if(j<nw)words[j].near=1;}
    }
    /* The raw c matrix is durable before any conflict/default-disable write. */
    char run[640],path[700];
    if(mkdir(argv[3],0700) && errno!=EEXIST)goto done;
    struct stat st;if(lstat(argv[3],&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0022))goto done;
    if(snprintf(run,sizeof(run),"%s/run-XXXXXX",argv[3])>=(int)sizeof(run) || !mkdtemp(run))goto done;
    snprintf(path,sizeof(path),"%s/original.json",run);if(joint_report(path,words,nw,0))goto done;
    const char *runid=strrchr(run,'/');runid=runid?runid+1:run;
    char previous[64]="";wake_settings_read(argv[3],"latest",previous,sizeof(previous));
    if(wake_settings_write(argv[3],"latest",runid) || wake_settings_write(run,"status","original"))goto done;
    joint_prune_reports(argv[3],runid,previous);
    crash_point("joint-after-original");
    if(joint_conf(argv[2],conf,words,nw,0))goto done;
    if(wake_settings_write(run,"status","disabled"))goto done;
    crash_point("joint-after-disable");
    for(int i=0;i<nw;i++) {
        int near=0;struct joint_word *w=&words[i];
        int bad=wake_joint_tighten(w->base,WAKE_AUTO_GATE_CEILING,WAKE_NORM_BAND,w->held,w->nheld,w->cross,w->ncross,&w->next,&near);
        /* Round downward to the actual six-decimal stored gate, then certify
         * all held-outs again; a rounding edge cannot accidentally widen it. */
        double scaled=(double)w->next*1000000.0;
        /* A stored six-decimal value can round to a float just below its
         * decimal representation. Keep it when it round-trips exactly. */
        float stored=(float)(round(scaled)/1000000.0);
        if(stored!=w->next)w->next=(float)(floor(scaled)/1000000.0);
        if(!(w->next>0))bad=1;
        for(int h=0;h<w->nheld;h++)if(w->held[h]>w->next){
            w->retry_mask|=1u<<w->held_record[h];bad=1;
        }
        if(!bad) {
            /* Tightening also tightens the quiet strong-ratio rule. Re-run
             * each held-out with the actual rounded proposed gate. */
            w->model.threshold=w->next; w->nheld=0;
            for(int r=1;r<=WAKE_ENROLL_LEGACY_MAX;r++) {
                char rec[512];snprintf(rec,sizeof(rec),"%s/recs/%s/%d.wav",argv[2],w->name,r);
                if(access(rec,F_OK))continue;
                if(joint_audio(w,rec,i,1)){bad=1;break;}
            }
            for(int h=0;h<w->nheld;h++)if(w->held[h]>w->next){
                w->retry_mask|=1u<<w->held_record[h];bad=1;
            }
        }
        if(bad){w->next=w->base;w->conflict=1;}else w->calibrated=1;
    }
    snprintf(path,sizeof(path),"%s/calibrated.json",run);if(joint_report(path,words,nw,1))goto done;
    if(joint_conf(argv[2],conf,words,nw,1) || wake_settings_write(run,"status","complete"))goto done;
    for(int i=0;i<nw;i++) {
        for(int r=1;r<=WAKE_ENROLL_LEGACY_MAX;r++)
            if(words[i].retry_mask&(1u<<r))printf("JOINT_REJECT %s self %d\n",words[i].name,r);
        if(words[i].cross_hit)printf("JOINT_REJECT %s cross\n",words[i].name);
    }
    printf("JOINT_SAVED %s original=original.json calibrated=calibrated.json\n",runid);rc=0;
done:
    if(rc)fprintf(stderr,"JOINT_FAILED: incomplete input or publication; no acoustic acceptance\n");
    free(conf);
    if(words){for(int i=0;i<nw;i++)for(uint32_t j=0;j<words[i].model.count;j++)free(words[i].model.items[j].feat);free(words);}
    if(mutation>=0)close(mutation);
    return rc;
}
