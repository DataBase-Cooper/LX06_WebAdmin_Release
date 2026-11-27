/* Complete A directory -> original B registry. No scheduler implementation. */
#include "object_schedule.h"
#include "../../../hooks/webd/app-schedule-directory.h"
static int directory_current(void *p){return eo_schedule_directory_current(p);}
static int directory_release(void *p){return eo_schedule_directory_unretain(p);}
int br_object_schedule_registry_init_directory(br_object_schedule_registry *g,br_schedule_runner *r,
    const br_object_schedule_binding *bindings,size_t count,struct eo_schedule_directory *directory){
    if(!g||g->ready||g->busy||!directory||eo_schedule_directory_count(directory)!=count||
       (count&&!bindings))return BR_OBJECT_INVALID;
    br_object_schedule_registry *next=calloc(1,sizeof(*next));if(!next)return BR_OBJECT_INVALID;
    int rc=BR_OBJECT_INVALID;g->busy=1;
    if(br_object_schedule_registry_init(next,r,bindings,count)!=1)goto done;
    for(size_t i=0;i<count;i++){
        const char *script;struct eo_owner *owner;struct eo_saved_source *source;
        if(eo_schedule_directory_at(directory,i,&script,&owner,&source)!=1||
           owner!=bindings[i].owner||source!=next->entries[i].source||strcmp(script,next->entries[i].script))goto done;
    }
    if(eo_schedule_directory_retain(directory)!=1)goto done;
    next->scope=directory;next->scope_current=directory_current;next->scope_release=directory_release;
    *g=*next;rc=BR_OBJECT_OK;
done:memset(next,0,sizeof(*next));free(next);g->busy=0;return rc;
}
