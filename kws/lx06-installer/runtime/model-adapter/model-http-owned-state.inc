/* Included beside the ORIGINAL transfer, only in the private owner build. */
struct mho_client {
    CURL *easy;
    mho_owner_current owner;
    void *context;
    uint64_t epoch,revision,last,deadline;
    char id[MHO_ID_BYTES],response[MHO_BYTES+1],ca[4096];
    size_t size;
    int allow_http,active,canceled,invalid,checking,borrowing;
    CURLcode code;
    long status;
};
static unsigned mho_clients;
static void mho_zero(void *memory,size_t size) {
    volatile unsigned char *p=memory;while(size--)*p++=0;
}
uint64_t mho_now(struct mho_client *c) {
    int64_t now=now_ms();
    if(now<=0||(c&&c->last&&(uint64_t)now<c->last))return 0;
    if(c)c->last=(uint64_t)now;
    return (uint64_t)now;
}
int mho_current(struct mho_client *c,uint64_t epoch,uint64_t revision) {
    /* Credential-owner callbacks borrow the exact client. Recursive checks
     * cannot acquire a second owner authority while that callback runs. */
    if(c&&c->borrowing){c->invalid=1;return -1;}
    if(!c||c->invalid||c->checking||epoch!=c->epoch||revision!=c->revision)return -1;
    c->checking=1;
    int failed=!mho_now(c)||!c->owner||c->owner(c->context,epoch,revision);
    c->checking=0;
    if(failed){c->invalid=1;return -1;}
    return c->invalid?-1:0;
}
static int mho_live(struct mho_client *c) {
    if(!c||!c->active||c->canceled||mho_current(c,c->epoch,c->revision)||c->canceled)return 0;
    uint64_t now=mho_now(c);return now&&now<c->deadline;
}
void mho_cancel(struct mho_client *c,const char *id) {
    if(c&&c->active&&id&&c->id[0]&&!strcmp(id,c->id))c->canceled=1;
}
