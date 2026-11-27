/* T02 local private captures. Uses the existing authenticated web88 handler.
 * Fixed <=100 clips/word/purpose, streaming export, no calibration side effects. */
#ifndef LX06_WAKE_COLLECTION_API_H
#define LX06_WAKE_COLLECTION_API_H
static int wake_collection_id(const char *s) {
    if (strlen(s) != 8 || strncmp(s, "s-", 2)) return 0;
    for (s += 2; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9'))) return 0;
    return 1;
}
static int wake_collection_file(int folder, const char *name, off_t limit) {
    struct stat st;
    int in = openat(folder, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (in < 0) return -1;
    if (fstat(in, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > limit) { close(in); return -1; }
    return in;
}
static int wake_collection_member(int fd, int folder, const char *id, const char *name) {
    unsigned char block[4096]; struct stat st; char member[100];
    int in = wake_collection_file(folder, name, 180000);
    if (in < 0) return -1;
    if (fstat(in, &st)) { close(in); return -1; }
    snprintf(member, sizeof(member), "%s/%s", id, name);
    wake_p05_tar_header(block, member, (unsigned)st.st_size);
    if (wake_p05_write(fd, block, 512)) { close(in); return -1; }
    off_t left = st.st_size;
    while (left) {
        ssize_t n = read(in, block, left > (off_t)sizeof(block) ? sizeof(block) : (size_t)left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || wake_p05_write(fd, block, (size_t)n)) { close(in); return -1; }
        left -= n;
    }
    close(in); memset(block, 0, 512);
    return wake_p05_write(fd, block, (512 - (size_t)st.st_size % 512) % 512);
}
/* Decode each exact query key, rejecting truncation, duplicate keys and NUL. */
static int wake_collection_query(const char *q, const char *key, char *out, size_t cap) {
    int found=0; size_t k=strlen(key), n=0;
    for (const char *p=q; p && *p;) {
        const char *end=strchr(p,'&'); if(!end)end=p+strlen(p);
        if ((size_t)(end-p)>k && !memcmp(p,key,k) && p[k]=='=') {
            if(found++)return -1;
            for(const char *v=p+k+1;v<end;v++) {
                unsigned char c=(unsigned char)*v;
                if(c=='%') {
                    if(end-v<3 || !isxdigit((unsigned char)v[1]) || !isxdigit((unsigned char)v[2]))return -1;
                    char hx[3]={v[1],v[2],0};c=(unsigned char)strtoul(hx,NULL,16);v+=2;
                } else if(c=='+')c=' ';
                if(!c || n+1>=cap)return -1;
                out[n++]=(char)c;
            }
        }
        p=*end?end+1:NULL;
    }
    out[n]=0;return found && n ? 0:-1;
}
static void wake_collection_get(int fd, const char *q, int exporting) {
#ifndef WW_VALIDATION_ROOT
#define WW_VALIDATION_ROOT "/tmp/lx06-wake-validation-samples"
#endif
#ifndef WW_STORE_PATH
#define WW_STORE_PATH(suffix) WWBASE suffix
#endif
    char name[24], purpose[16], path[256], json[256];
    int count = 0, normal = 0, far = 0, soft = 0, failed = 0;
    if (wake_collection_query(q,"name",name,sizeof(name)) || !ww_name_ok(name) ||
        wake_collection_query(q,"purpose",purpose,sizeof(purpose)) ||
        (strcmp(purpose,"acceptance") && strcmp(purpose,"calibration") && strcmp(purpose,"diagnostic"))) {
        app_http_error(fd,400,"独立采集用途或词名无效"); return;
    }
    /* New diagnostic audio is transient. Export the old collection only
     * when there is no transient collection for this word/purpose. */
    snprintf(path,sizeof(path),"%s/samples/%s/%s",WW_VALIDATION_ROOT,purpose,name);
    if (access(path, F_OK) != 0 && errno == ENOENT)
        snprintf(path,sizeof(path),"%s/samples/%s/%s",WW_STORE_PATH(""),purpose,name);
    int base = open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR *dir = base < 0 ? NULL : fdopendir(base);
    if (base >= 0 && !dir) close(base);
    if (!dir && errno != ENOENT) { app_http_error(fd,500,"无法读取独立样本"); return; }
    if (exporting) {
        const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/x-tar\r\nContent-Disposition: attachment; filename=\"lx06-private-samples.tar\"\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        if (wake_p05_write(fd,hdr,strlen(hdr))) { if (dir) closedir(dir); return; }
    }
    struct dirent *entry;
    while (dir && count < 100 && (entry = readdir(dir))) {
        char metadata[768], condition[16], digest[65], label[24], stored_purpose[16], stored_id[16];
        struct stat ready;
        if (!wake_collection_id(entry->d_name)) continue;
        int folder = openat(dirfd(dir),entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if (folder < 0) continue;
        if (fstatat(folder,"ready",&ready,AT_SYMLINK_NOFOLLOW) || !S_ISREG(ready.st_mode)) { close(folder); continue; }
        int in = wake_collection_file(folder,"sample.json",sizeof(metadata)-1);
        int wav = wake_collection_file(folder,"audio.wav",180000);
        if (in < 0 || wav < 0) { if(in>=0)close(in); if(wav>=0)close(wav); close(folder); continue; }
        close(wav); ssize_t n = read(in,metadata,sizeof(metadata)-1); close(in);
        if (n <= 0) { close(folder); continue; } metadata[n]=0;
        if (app_json_str(metadata,"condition",condition,sizeof(condition)) ||
            app_json_str(metadata,"sha256",digest,sizeof(digest)) || strlen(digest)!=64 || strspn(digest,"0123456789abcdef")!=64 ||
            app_json_str(metadata,"label",label,sizeof(label)) || strcmp(label,name) ||
            app_json_str(metadata,"purpose",stored_purpose,sizeof(stored_purpose)) || strcmp(stored_purpose,purpose) ||
            app_json_str(metadata,"id",stored_id,sizeof(stored_id)) || strcmp(stored_id,entry->d_name) ||
            (strcmp(condition,"normal") && strcmp(condition,"far") && strcmp(condition,"soft"))) { close(folder); continue; }
        count++; normal += !strcmp(condition,"normal"); far += !strcmp(condition,"far"); soft += !strcmp(condition,"soft");
        /* Export under word/id so sample.json's path stays usable after extraction. */
        char prefix[64]; snprintf(prefix,sizeof(prefix),"%s/%s",name,entry->d_name);
        if (exporting && (wake_collection_member(fd,folder,prefix,"sample.json") || wake_collection_member(fd,folder,prefix,"audio.wav"))) failed=1;
        close(folder); if(failed)break;
    }
    if (dir) closedir(dir);
    if (exporting) { if(!failed) { unsigned char end[1024]={0}; wake_p05_write(fd,end,sizeof(end)); } return; }
    snprintf(json,sizeof(json),"{\"ok\":true,\"count\":%d,\"normal\":%d,\"far\":%d,\"soft\":%d,\"acceptance_passed\":false}",count,normal,far,soft);
    send_json(fd,json);
}
#endif
