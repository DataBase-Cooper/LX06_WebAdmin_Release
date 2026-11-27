/* Optional upload-embedded protocol adapter; never replaces a ROM component.
 * Website patch SSE -> the existing model-voice NDJSON delta contract.
 * Fixed memory, immediate flush, no reasoning/TIP output, no success on EOF.
 * All HTTP/authentication/challenge policy remains in the shell upload.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "../doubao/src/json.c"
#define LIMIT 16384
struct fragment { char content[LIMIT+1]; int spoken; };
static struct fragment fragments[16];
static int count,current=-1,finished,collect,total;
static char answer[LIMIT+1],path[256],operation[24]="SET";
static const char *error_file;
/* Only an explicitly structured numeric server error is retained. No token,
 * raw upstream text, identifiers or model content can enter this diagnostic.
 * Shell policy turns these codes into replaceable, user-readable notices. */
static int server_error(const xz_json_doc *d) {
    int code=0,biz=0,data=xz_json_object_get(d,0,"data");
    if(xz_json_get_int(d,0,"code",&code))return 0;
    int business=data>=0?xz_json_object_get(d,data,"biz_data"):-1;
    if(data>=0)xz_json_get_int(d,data,"biz_code",&biz);
    if(!code&&!biz)return 0;
    int muted=0;
    if(business>=0){
        if(xz_json_get_int(d,business,"is_muted",&muted)){
            int t=xz_json_object_get(d,business,"is_muted");
            muted=t>=0&&d->tokens[t].end-d->tokens[t].start==4&&!memcmp(d->source+d->tokens[t].start,"true",4);
        }
        muted=muted==1;
    }
    int until=0;
    if(data>=0){
        /* Current server uses fractional epoch seconds. Copy only the integer
         * portion after checking its entire primitive, never arbitrary text. */
        int t=xz_json_object_get(d,business>=0?business:data,"mute_until");
        if(t>=0&&d->tokens[t].type==XZ_JSON_PRIMITIVE){
            char value[32];size_t n=d->tokens[t].end-d->tokens[t].start;
            if(n>0&&n<sizeof(value)){
                memcpy(value,d->source+d->tokens[t].start,n);value[n]=0;
                char *end;double v=strtod(value,&end);
                if(!*end&&v>0&&v<=2147483647)until=(int)v;
            }
        }
    }
    if(error_file){
        int fd=open(error_file,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        if(fd>=0){
            dprintf(fd,"{\"code\":%d,\"data\":{\"biz_code\":%d,\"mute_until\":%d,\"biz_data\":{\"is_muted\":%d,\"mute_until\":%d}}}",code,biz,until,muted,until);
            close(fd);
        }
    }
    return -1;
}
static void quote(const char *s) {
    putchar('"');
    for(const unsigned char *p=(const unsigned char*)s;*p;p++) {
        if(*p=='"'||*p=='\\'){putchar('\\');putchar(*p);}
        else if(*p<32)printf("\\u%04x",*p);else putchar(*p);
    }
    putchar('"');
}
static int emit(const char *text) {
    size_t n=strlen(text);
    if(total+(int)n>LIMIT)return -1;
    memcpy(answer+total,text,n+1);total+=(int)n;
    if(!collect&&n){fputs("{\"delta\":",stdout);quote(text);puts("}");fflush(stdout);}
    return ferror(stdout)?-1:0;
}
static int content(int index,const char *text,int append) {
    if(index<0||index>=count)return -1;
    struct fragment *f=&fragments[index];size_t old=strlen(f->content),n=strlen(text);
    if(append) {
        if(old+n>LIMIT)return -1;
        memcpy(f->content+old,text,n+1);
        return f->spoken?emit(text):0;
    }
    /* Snapshot replay may only extend already-spoken content. Never repeat it
     * or narrate a rewritten answer that conflicts with audio already played. */
    if(n>LIMIT||(f->spoken&&(n<old||memcmp(f->content,text,old))))return -1;
    if(f->spoken&&emit(text+old))return -1;
    memcpy(f->content,text,n+1);return 0;
}
static int fragment_array(const xz_json_doc *d,int token,int append) {
    if(token<0||d->tokens[token].type!=XZ_JSON_ARRAY)return -1;
    int index=append?count:0;
    for(int v=d->tokens[token].first_child;v>=0;v=d->tokens[v].next_sibling,index++) {
        char type[32],text[LIMIT+1];
        if(index>=16||xz_json_get_string(d,v,"type",type,sizeof(type))||xz_json_get_string(d,v,"content",text,sizeof(text)))return -1;
        int spoken=!strcmp(type,"RESPONSE");
        if(index>=count){memset(&fragments[index],0,sizeof(fragments[index]));count=index+1;fragments[index].spoken=spoken;}
        if(fragments[index].spoken!=spoken||content(index,text,0))return -1;
        current=index;
    }
    return 0;
}
static int patch(const xz_json_doc *d,int root,const char *base,int depth) {
    if(depth>6||root<0||d->tokens[root].type!=XZ_JSON_OBJECT)return -1;
    char localpath[256],op[24],full[256];int p=xz_json_object_get(d,root,"p");
    if(p>=0) {
        if(decode_string(d,p,localpath,sizeof(localpath)))return -1;
        if(*base){if(snprintf(full,sizeof(full),"%s/%s",base,localpath)>=(int)sizeof(full))return -1;}
        else {memcpy(full,localpath,strlen(localpath)+1);}
        if(!depth)memcpy(path,full,strlen(full)+1);
    } else {memcpy(full,path,strlen(path)+1);}
    if(xz_json_get_string(d,root,"o",op,sizeof(op)))strcpy(op,operation);
    else if(!depth)strcpy(operation,op);
    int v=xz_json_object_get(d,root,"v");if(v<0)return 0;
    if(!strcmp(op,"BATCH")&&d->tokens[v].type==XZ_JSON_ARRAY) {
        for(int t=d->tokens[v].first_child;t>=0;t=d->tokens[t].next_sibling)if(patch(d,t,full,depth+1))return -1;
        return 0;
    }
    if(d->tokens[v].type==XZ_JSON_OBJECT) {
        int response=xz_json_object_get(d,v,"response");
        if(response<0&&!strcmp(full,"response"))response=v;
        if(response>=0) {
            int fs=xz_json_object_get(d,response,"fragments");
            if(fs>=0&&fragment_array(d,fs,0))return -1;
            char status[32];if(!xz_json_get_string(d,response,"status",status,sizeof(status))&&!strcmp(status,"FINISHED"))finished=1;
        }
        return 0;
    }
    if(!strcmp(full,"response/fragments"))return fragment_array(d,v,!strcmp(op,"APPEND"));
    if(d->tokens[v].type==XZ_JSON_STRING) {
        char text[LIMIT+1];if(decode_string(d,v,text,sizeof(text)))return -1;
        if(!strcmp(full,"response/status")) {
            if(!strcmp(text,"FINISHED"))finished=1;
            else if(strcmp(text,"WIP"))return -1;
        } else if(!strncmp(full,"response/fragments/",19)) {
            const char *index=full+19;int i=-1;
            if(!strcmp(index,"-1/content"))i=current;
            else if(*index>='0'&&*index<='9') {
                i=0;while(*index>='0'&&*index<='9'){i=i*10+(*index++-'0');if(i>=16)return -1;}
                if(strcmp(index,"/content"))return 0;
            } else return 0;
            if(content(i,text,!strcmp(op,"APPEND")))return -1;
        }
    }
    return 0;
}
static int event(const char *name,char *data) {
    xz_json_doc d;if(xz_json_parse(&d,data,strlen(data)))return -1;
    if(server_error(&d))return -1;
    if(!strcmp(name,"error")||!strcmp(name,"fail"))return -1;
    if(!*name||!strcmp(name,"message"))return patch(&d,0,"",0);
    /* Metadata never changes the inherited patch path/operation. */
    if(!strcmp(name,"ready")||!strcmp(name,"update_session")||!strcmp(name,"title")||!strcmp(name,"close"))return 0;
    return -1;
}
static int stream(void) {
    char line[32768],data[32768]="",name[64]="";size_t used=0,wire=0;
    while(fgets(line,sizeof(line),stdin)) {
        size_t n=strlen(line);wire+=n;if(wire>1048576||(!strchr(line,'\n')&&!feof(stdin)))return 3;
        while(n&&(line[n-1]=='\n'||line[n-1]=='\r'))line[--n]=0;
        if(!n) {
            if(used&&event(name,data))return 3;
            used=0;data[0]=0;name[0]=0;continue;
        }
        if(!strncmp(line,"event:",6)){const char *s=line+6;if(*s==' ')s++;if(strlen(s)>=sizeof(name))return 3;strcpy(name,s);}
        else if(!strncmp(line,"data:",5)) {
            const char *s=line+5;if(*s==' ')s++;size_t z=strlen(s);
            if(used+z+2>=sizeof(data))return 3;
            if(used)data[used++]='\n';memcpy(data+used,s,z+1);used+=z;
        } else if(*line!=':'&&strncmp(line,"id:",3)&&strncmp(line,"retry:",6)){
            /* HTTP 200 can contain a non-SSE JSON refusal (e.g. MUTED=50006).
             * Recognize that refusal; never treat JSON text as answer deltas. */
            xz_json_doc d;
            if(used||xz_json_parse(&d,line,n))return 3;
            server_error(&d);return 3;
        }
    }
    if(ferror(stdin)||(used&&event(name,data))||!finished||!total)return 3;
    if(collect){
        if(collect==2&&total>1800){int n=1800;while(n&&((unsigned char)answer[n]&0xc0)==0x80)n--;answer[n]=0;}
        fputs(collect==2?"{\"message\":":"{\"text\":",stdout);quote(answer);puts("}");
    }
    return 0;
}
static int prompt(void) {
    char input[32768];size_t n=fread(input,1,sizeof(input)-1,stdin);input[n]=0;
    if(ferror(stdin)||!feof(stdin))return 3;
    xz_json_doc d;if(xz_json_parse(&d,input,n))return 3;
    int a=xz_json_object_get(&d,0,"input");if(a<0||d.tokens[a].type!=XZ_JSON_ARRAY)return 3;
    int turns=0;
    for(int t=d.tokens[a].first_child;t>=0;t=d.tokens[t].next_sibling)turns++;
    if(!turns||turns>7)return 3;
    if(turns>1)fputs("请接着以下对话回答最后一个用户问题，只输出回答正文。\n",stdout);
    int lastuser=0;
    for(int t=d.tokens[a].first_child;t>=0;t=d.tokens[t].next_sibling) {
        char role[32],text[4096];
        if(xz_json_get_string(&d,t,"role",role,sizeof(role))||xz_json_get_string(&d,t,"content",text,sizeof(text)))return 3;
        lastuser=!strcmp(role,"user");if(!lastuser&&strcmp(role,"assistant"))return 3;
        if(turns>1)fputs(lastuser?"\n用户：":"\n助手：",stdout);
        fputs(text,stdout);
    }
    return lastuser&&!ferror(stdout)?0:3;
}
int main(int argc,char **argv) {
    if(argc!=2&&argc!=4)return 2;
    if(argc==4){if(strcmp(argv[2],"--error-file"))return 2;error_file=argv[3];}
    if(!strcmp(argv[1],"--prompt"))return prompt();
    if(!strcmp(argv[1],"--collect"))collect=1;
    else if(!strcmp(argv[1],"--preview"))collect=2;
    else if(strcmp(argv[1],"--stream"))return 2;
    return stream();
}
