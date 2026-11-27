/* Authenticated, stateless batch log tail. Client cursors carry source identity
 * and a hash of bytes immediately before the offset, detecting truncate/rewrite
 * even when the replacement has already grown past the old offset. No daemon,
 * shell command, DATA writes or per-script HTTP polling is needed. */
static unsigned long feed_hash(const char *p, size_t n) {
  unsigned long h=2166136261UL;
  while(n--) h=((h^(unsigned char)*p++)*16777619UL)&0xffffffffUL;
  return h;
}
static unsigned long feed_anchor(int fd,long off) {
  char b[64]; size_t n=off>64?64:(size_t)off;
  if(lseek(fd,off-(long)n,SEEK_SET)<0 || read(fd,b,n)!=(ssize_t)n) return 0;
  return feed_hash(b,n);
}
static void h_apps_log_feed(int fd,const char *body) {
  const size_t cap=512*1024;
  char *js=malloc(cap); size_t used=0; int count=0;
  DIR *dir; struct dirent *de;
  if(!js) { send_err(fd,503,"Busy","{\"ok\":false}"); return; }
  used=(size_t)snprintf(js,cap,"{\"ok\":true,\"entries\":[");
  dir=opendir(APPPERSIST);
  while(dir && (de=readdir(dir))!=NULL && count<64) {
    char paths[2][560], key[80], prev[160], cursor[160];
    if(!app_name_safe(de->d_name)) continue;
    log_path_for(de->d_name,paths[0],sizeof(paths[0]));
    app_runlog_path(de->d_name,paths[1],sizeof(paths[1]));
    for(int kind=0;kind<2 && count<64;kind++) {
      char buf[2049], esc[12300], name[400]; LX06_STATBUF st;
      unsigned long id,oldid=0,anchor=0; long off=0; int reset=0;
      if(kind && !strcmp(paths[0],paths[1])) continue;
      int lf=app_log_open(paths[kind],O_RDONLY);
      if(lf<0) continue;
      if(LX06_FSTAT(lf,&st)!=0 || !S_ISREG(st.st_mode)) {close(lf);continue;}
      snprintf(key,sizeof(key),"%s:%d",de->d_name,kind);
      prev[0]=0; app_json_str(body,key,prev,sizeof(prev));
      id=feed_hash(paths[kind],strlen(paths[kind]))^(unsigned long)st.st_ino;
      if(sscanf(prev,"%lx:%ld:%lx",&oldid,&off,&anchor)!=3) {
        /* Initial view shows a bounded recent tail, then only new bytes. */
        off=st.st_size>2048?(long)st.st_size-2048:0;
      } else if(oldid!=id || off<0 || off>st.st_size || feed_anchor(lf,off)!=anchor) {
        off=0;reset=1;
      }
      lseek(lf,off,SEEK_SET); ssize_t got=read(lf,buf,2048);
      if(got<0) {close(lf);continue;}
      /* Never split a UTF-8 character across JSON responses. */
      if(got && off+got<st.st_size) {
        ssize_t start=got-1;
        while(start>0 && ((unsigned char)buf[start]&0xc0)==0x80) start--;
        unsigned char c=(unsigned char)buf[start];
        int need=c>=0xf0?4:c>=0xe0?3:c>=0xc0?2:1;
        if(got-start<need) got=start;
      }
      size_t skip=0;
      if(!prev[0] && off) while(skip<(size_t)got && ((unsigned char)buf[skip]&0xc0)==0x80) skip++;
      off+=got;
      snprintf(cursor,sizeof(cursor),"%lx:%ld:%lx",id,off,feed_anchor(lf,off));close(lf);
      json_escape(buf+skip,(size_t)got-skip,esc,sizeof(esc));
      json_escape(de->d_name,strlen(de->d_name),name,sizeof(name));
      if(used+strlen(esc)+1024>=cap) break;
      used+=(size_t)snprintf(js+used,cap-used,"%s{\"key\":\"%s\",\"name\":\"%s\",\"cursor\":\"%s\",\"reset\":%d,\"copy_codes\":%d,\"data\":\"%s\"}",count?",":"",key,name,cursor,reset,!!(app_file_marker_flags(de->d_name)&APP_COPY_CODES),esc);
      count++;
    }
  }
  if(dir) closedir(dir);
  snprintf(js+used,cap-used,"]}");send_json(fd,js);free(js);
}
