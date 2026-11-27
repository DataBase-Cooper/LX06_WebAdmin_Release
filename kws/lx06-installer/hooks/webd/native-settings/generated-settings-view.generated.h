#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "generated settings view is host fixture only"
#endif
static int gsv_put(struct gsv_buf *b,const char *s) { return gsv_bytes(b,s,strlen(s)); }
static int gsv_quote(struct gsv_buf *b,const char *s) {
    if(gsv_put(b,"\"")) return -1;
    for(;*s;s++) {
        unsigned char c=(unsigned char)*s; char x[7];
        if(c=='"'||c=='\\') { x[0]='\\'; x[1]=(char)c; x[2]=0; }
        else if(c<32) snprintf(x,sizeof(x),"\\u%04x",c);
        else { x[0]=(char)c; x[1]=0; }
        if(gsv_put(b,x)) return -1;
    }
    return gsv_put(b,"\"");
}
static int gsv_number(struct gsv_buf *b,unsigned n) {
    char value[16]; snprintf(value,sizeof(value),"%u",n); return gsv_put(b,value);
}

static int gsv_public(const struct cfg *c,const struct ctl_map *map,struct gsv_buf *b) {
    /* One cfg parser supplies both metadata and per-App views. No second scan
     * of the uploaded script, and no raw endpoint/secret in public JSON. */
    struct cfg_metadata metadata;
    if(cfg_metadata_view(c,&metadata)) return -1;
    unsigned start,maximum; cfg_channel_timing(c,&start,&maximum);
    const char *local=cfg_value(c,"LX06-LOCAL-CHANNEL"),*official=cfg_value(c,"小爱通道唤醒词");
    if(gsv_put(b,"{\"version\":1,\"status\":\"offline_unverified\",\"channel\":{\"local\":")||
       gsv_quote(b,local?local:"")||gsv_put(b,",\"official\":")||gsv_quote(b,official?official:"")||
       gsv_put(b,",\"start_ms\":")||gsv_number(b,start)||gsv_put(b,",\"max_ms\":")||gsv_number(b,maximum)||
       gsv_put(b,"},\"apps\":[")) return -1;
    for(size_t a=0;a<c->app_count;a++) {
        /* Same declaration view as the future bridge consumer. Validate local
         * syntax before constructing output/renaming a saved record. Session
         * kind is unknown offline, so caps-specific exit requirements stay
         * pending; never infer them from an App name or the mic setting. */
        struct cfg_bridge_app declared;
        if(cfg_bridge_app_view(c,c->apps[a],CFG_SESSION_SILENT,&declared)) return -1;
        struct cfg_control view=declared.control;
        if((a&&gsv_put(b,","))||gsv_put(b,"{\"app\":")||gsv_quote(b,c->apps[a])||gsv_put(b,",\"rows\":[")) return -1;
        for(size_t i=0;;i++) {
            const char *row=cfg_ctl_row(c,c->apps[a],i); if(!row) break;
            if((i&&gsv_put(b,","))||gsv_quote(b,row)) return -1;
        }
        if(gsv_put(b,"],\"start_ms\":")||gsv_number(b,view.start_ms)||gsv_put(b,",\"max_ms\":")||gsv_number(b,view.max_ms)||
           gsv_put(b,",\"timeout\":")||gsv_quote(b,view.end_on_timeout?"end":"resume")||
           gsv_put(b,",\"bargein\":")||gsv_quote(b,view.bargein)||gsv_put(b,",\"exit\":")||gsv_quote(b,view.exit)||
           gsv_put(b,",\"hangup\":")||gsv_quote(b,view.hangup)||gsv_put(b,"}")) return -1;
    }
    if(gsv_put(b,"],\"overridden\":[")) return -1;
    for(size_t i=0;i<map->count;i++) if((i&&gsv_put(b,","))||gsv_quote(b,map->keys[i])) return -1;
    return gsv_put(b,"]}");
}
