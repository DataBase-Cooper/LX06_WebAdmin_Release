/* Private typed composition of the original CTL compiler. The frozen scalar
 * header, structures and entry points retain their original contract. */
#ifndef LX06_CTL_LIST_APPROVAL_H
#define LX06_CTL_LIST_APPROVAL_H
#include "ctl-config.h"
struct expr_program;
struct expr_list_bounds;
/* Separate startup authority: no synthetic CTL row or operation is accepted.
 * The original parser supplies the entire cfg and exact channel candidate. */
typedef int (*cfg_startup_approve)(void *,const struct cfg *,const char *,unsigned,
    const struct expr_program *,struct expr_list_bounds *);
/* Approval follows original syntax validation. The retained source owner must
 * prove each list slot and its entire mapping; this grants no execution rights. */
/* Private local approval flag, never serialized or a matcher capture bit.
 * Missing RHS references require same-source explicit completion proof. */
#define CFG_ROW_COMPLETION (1U<<31)
typedef int (*cfg_list_approve)(void *,const char *,const char *,const struct cfg_row *,unsigned);
int cfg_parse_script_startup(const char *,size_t,struct cfg *,size_t *,cfg_list_approve,void *,cfg_startup_approve);
int cfg_effective_startup(const struct cfg *,const struct cfg_override *,size_t,struct cfg *,cfg_list_approve,void *,cfg_startup_approve);
int cfg_validate_startup_channels(const struct cfg *,cfg_startup_approve,void *);
struct decl_table;
int cfg_parse_startup(const char *,size_t,struct cfg *,cfg_list_approve,void *,cfg_startup_approve);
int decl_read_fd_startup(int,struct cfg *,cfg_list_approve,void *,cfg_startup_approve);
int decl_compile_startup(const struct cfg *,const char *,struct decl_table *,cfg_list_approve,void *,cfg_startup_approve);
int decl_compile_entry_channels_startup(const struct cfg *,struct decl_table *,cfg_startup_approve,void *);
int cfg_parse_row_scoped(const char *,const char *,struct cfg_row *,cfg_list_approve,void *);
int cfg_parse_rows(const char *,size_t,struct cfg *,cfg_list_approve,void *);
int cfg_effective_rows(const struct cfg *,const struct cfg_override *,size_t,struct cfg *,cfg_list_approve,void *);
/* Same original complete-script upload parser loop, with scoped list proof. */
int cfg_parse_script_rows(const char *,size_t,struct cfg *,size_t *,cfg_list_approve,void *);
#endif
