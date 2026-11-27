static int rts_ext_header(char *head,struct ext_schema*s) {
    int active=0;size_t used=0;s->json[0]=0;
    char*p=head;
    while(*p) {
        char*end=strchr(p,'\n'); if(!end)return active?-1:0;
        *end=0; if(end>p&&end[-1]=='\r')end[-1]=0;
        while(*p==' '||*p=='\t')p++;
        if(!strcmp(p,"# LX06-EXTENSION-BEGIN")) { if(active)return -1; active=1; }
        else if(!strcmp(p,"# LX06-EXTENSION-END")) return active?ext_parse(s):-1;
        else if(active) {
            if(*p!='#')return -1;
            p++; if(*p==' ')p++;
            size_t z=strlen(p); if(used+z+1>EXT_CAP)return -1;
            memcpy(s->json+used,p,z); used+=z; s->json[used++]='\n'; s->json[used]=0;
        } else if(*p&&*p!='#')return 0;
        p=end+1;
    }
    return active?-1:0;
}
