#ifndef DPE_ORACLE_H
#define DPE_ORACLE_H
#include "globals/global_types.h"
#ifdef __cplusplus
extern "C" {
#endif
void dpo_init(void);
Flag dpo_predict(Op* op, Flag realistic);
Flag dpo_gate(Op* op);
void dpo_issue(Op* op);
void dpo_resolve(Op* op);
void dpo_release(Op* op);
void dpo_advance(uns proc_id);
void dpo_end_cycle(uns proc_id);
void dpo_start_roi(uns proc_id);
void dpo_finish(uns proc_id);
Flag dpo_frontend_stalled(uns proc_id);
void dpo_frontend_check(Op* op);
#ifdef __cplusplus
}
#endif
#endif
