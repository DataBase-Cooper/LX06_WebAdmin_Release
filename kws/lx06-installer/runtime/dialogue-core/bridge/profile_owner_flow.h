/* Private selected-source FLOW composition. All objects have fixed addresses
 * through original drain/retirement. No production registration or new wire. */
#ifndef LX06_BR_PROFILE_OWNER_FLOW_H
#define LX06_BR_PROFILE_OWNER_FLOW_H
#ifndef LX06_SELECTED_FLOW_PROFILE_FIXTURE
#error "selected FLOW owner requires explicit fixture admission"
#endif
#include "profile_owner.h"
#include "object_flow.h"
typedef struct br_profile_flow {
    br_profile_owner *owner;
    struct eo_saved_source *source;
    br_flow flow;br_flow_session adapter;br_object_flow_run run;
    br_object_source_current current;void *context;
    unsigned char wire[BR_FLOW_SESSION_WIRE];
} br_profile_flow;
/* After original CAPS/session; caller initializes original log/store using
 * this exact adapter wire. No immutable object is copied or moved. */
int br_profile_owner_flow_init(br_profile_flow *,br_profile_owner *,struct eo_saved_source *);
int br_profile_owner_flow_start(br_profile_flow *,br_object_flow *,const char *,
    br_flow_session_log *,br_flow_owner_store *,uint64_t,br_object_source_current,void *,
    br_flow_owner_handoff_current,void *);
int br_profile_owner_flow_dispatch(br_profile_flow *,uint64_t,uint64_t,uint64_t,const char *);
int br_profile_owner_flow_poll(br_profile_flow *,uint64_t,uint64_t,int);
int br_profile_owner_flow_result(br_profile_flow *,br_inbox *,uint64_t,int,br_result *,br_result_progress *);
int br_profile_owner_flow_retire(br_profile_flow *,br_flow_session_log_closed,void *);
#endif
