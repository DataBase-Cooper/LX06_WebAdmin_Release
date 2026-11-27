/* The same perform_internal executes owned requests and the original CLI.
 * This interface never creates a listener, subprocess or request directory. */
static void mho_library_close(void) {
    if(curl.global_cleanup)curl.global_cleanup();
    if(curl.library)dlclose(curl.library);
    memset(&curl,0,sizeof(curl));
}
struct mho_client *mho_create(mho_owner_current current,void *opaque,uint64_t epoch,
        uint64_t revision,const char *ca,int allow_http) {
    if(!current||(allow_http!=0&&allow_http!=1)||(ca&&strlen(ca)>=4096))return NULL;
    /* Do not take over an existing CLI server's transport or global library. */
    if(!mho_clients&&curl.library)return NULL;
    if(!mho_clients&&!load_curl()) {
        if(curl.library)dlclose(curl.library);
        memset(&curl,0,sizeof(curl));return NULL;
    }
    struct mho_client *c=calloc(1,sizeof(*c));
    if(c)c->easy=curl.easy_init();
    if(!c||!c->easy){free(c);if(!mho_clients)mho_library_close();return NULL;}
    c->owner=current;c->context=opaque;c->epoch=epoch;c->revision=revision;c->allow_http=allow_http;
    if(ca)strcpy(c->ca,ca);
    mho_clients++;
    if(mho_current(c,epoch,revision)){mho_destroy(c);return NULL;}
    return c;
}
int mho_destroy(struct mho_client *c) {
    if(!c)return 0;
    if(c->borrowing){c->invalid=1;return -1;}
    if(c->active||c->checking)return -1;
    if(c->easy)curl.easy_cleanup(c->easy);
    mho_zero(c,sizeof(*c));free(c);
    if(!--mho_clients)mho_library_close();
    return 0;
}
/* The adapter invokes original credential-owner callbacks under this narrow
 * synchronous borrow. No copy of the account/config authority is created. */
int mho_owner_call(struct mho_client *c,int (*callback)(void *),void *context) {
    if(c&&c->borrowing){c->invalid=1;return -1;}
    if(!c||!callback||c->invalid||c->active||c->checking)return -1;
    c->borrowing=1;int rc=callback(context);c->borrowing=0;
    return c->invalid?-1:rc;
}
int mho_post(struct mho_client *c,const char *id,const char *url,const char *headers,
        const char *body,size_t length,unsigned timeout,uint64_t deadline,
        char out[MHO_BYTES+1],size_t *size,long *http_status) {
    if(!out||!size||!http_status)return -1;
    out[0]=0;*size=0;*http_status=0;
    if(c&&c->borrowing){c->invalid=1;return -1;}
    if(!c||!c->easy||c->active||c->checking||!id||!id[0]||strnlen(id,MHO_ID_BYTES)>=MHO_ID_BYTES||
       !url||strnlen(url,URL_LIMIT+1)>URL_LIMIT||!headers||strnlen(headers,HEADER_LIMIT+1)>HEADER_LIMIT||
       !body||!length||length>MHO_BYTES||memchr(body,0,length)||!timeout||timeout>10000)return -1;
    uint64_t now=mho_now(c);
    if(!now||deadline<=now||deadline-now>timeout)return -1;
    struct request r={0};r.method=strdup("POST");r.url=strdup(url);r.headers=strdup(headers);
    r.body=malloc(length+1);r.lens[0]=4;r.lens[1]=(uint32_t)strlen(url);
    r.lens[2]=(uint32_t)strlen(headers);r.lens[3]=(uint32_t)length;
    /* The unchanged CLI validator owns method/URL/header framing. Its seconds
     * field is valid scaffolding; owned execution below uses only deadline ms. */
    r.timeout=1;
    if(r.body){memcpy(r.body,body,length);r.body[length]=0;}
    int rc=-1;
    if(!request_valid(&r,c->allow_http))goto done;
    c->active=1;c->canceled=0;c->deadline=deadline;c->size=0;c->status=0;c->code=CURLE_FAILED_INIT;
    c->response[0]=0;strcpy(c->id,id);
    if(!mho_live(c))goto finish;
    int failed=perform_internal(-1,c->easy,&r,c->allow_http,c->ca[0]?c->ca:NULL,c);
    *http_status=c->status;
    if(!failed&&c->code==CURLE_OK&&c->status>=200&&c->status<300&&mho_live(c)) {
        memcpy(out,c->response,c->size+1);*size=c->size;rc=0;
    }
    if(rc) {
        /* A canceled or failed response never lends its incomplete connection
         * to the next independent request, matching the original server. */
        curl.easy_cleanup(c->easy);c->easy=curl.easy_init();
    }
finish:
    c->active=0;c->id[0]=0;c->deadline=0;mho_zero(c->response,sizeof(c->response));c->size=0;
done:
    if(r.headers)mho_zero(r.headers,r.lens[2]);
    if(r.body)mho_zero(r.body,r.lens[3]);
    free_request(&r);return rc;
}
