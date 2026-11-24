#ifndef BR_LD_OVERLAP_H
#define BR_LD_OVERLAP_H
#include "globals/global_types.h"
#ifdef __cplusplus
extern "C" {
#endif
void brld_init(void);
void brld_mapped(Op* op);
void brld_dispatch(Op* op);
void brld_issue(Op* op);
void brld_ready(Op* op);
void brld_llc_miss(Op* op, Flag merged);
void brld_resolve(Op* op);
void brld_recover(Op* op);
void brld_release(Op* op);
void brld_start_roi(uns proc_id);
void brld_finish(uns proc_id);
#ifdef __cplusplus
}
#endif
#endif
