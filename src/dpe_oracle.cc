#include "dpe_oracle_model.h"
#include "dpe_serial_model.h"
#include <cstdio>
#include <memory>
#include <string>
extern "C" {
#include "globals/global_defs.h"
#include "globals/global_vars.h"
#include "globals/assert.h"
#include "core.param.h"
#include "general.param.h"
#include "bp/bp.h"
#include "bp/bp.param.h"
#include "frontend/frontend_intf.h"
#include "op.h"
#include "sim.h"
#include "freq.h"
#include "statistics.h"
#include "dpe_oracle.h"
}
namespace {
struct Counts {
  Counter compared=0, mismatch=0, created=0, resolved=0, canceled=0, carry=0;
  Counter active_cycles=0, gate_cycles=0, gated_ops=0, issued_younger=0, issued_loads=0;
};
struct Core {
  dpo::Barriers barriers;
  dpo::Serial serial;
  Counter serial_counts[12]={};
  Counts n;
  bool roi=false, finished=false;
  Counter instructions=0, gated_this_cycle=0, last_advance=dpo::never;
  unsigned tests=0;
  FILE* events=nullptr;
};
std::vector<std::unique_ptr<Core>> cores;
Core* state(uns p) {
  return p < cores.size() && !cores[p]->finished ? cores[p].get() : nullptr;
}
void event(Core* c, uns proc, const char* name, Counter order, Counter uid,
           Counter generation, Counter value=0, Counter pc=0) {
  if (!c || !c->roi || !c->events) return;
  fprintf(c->events, "{\"event\":\"%s\",\"cycle\":%llu,\"proc\":%u,\"order\":%llu,"
                    "\"uid\":%llu,\"generation\":%llu,\"value\":%llu,\"pc\":%llu}\n",
          name, (unsigned long long)cycle_count, proc, (unsigned long long)order,
          (unsigned long long)uid, (unsigned long long)generation,
          (unsigned long long)value, (unsigned long long)pc);
}
void serial_events(Core* c, uns p, const std::vector<dpo::SerialEvent>& events) {
  // First seven counters correspond to SerialKind in the standalone model.
  static const char* names[]={"serial_start","serial_stop","serial_end","serial_cancel",
    "serial_pending_cancel","serial_handoff","serial_handoff_resolved"};
  for (const auto& e:events) {
    unsigned k=static_cast<unsigned>(e.kind);
    if (c->roi) ++c->serial_counts[k];
    event(c,p,names[k],e.slot.order,e.slot.unique,e.slot.generation,e.other);
    // Canceling an active root also invalidates its younger pending barrier.
    if ((e.kind==dpo::SerialKind::PendingCancel || e.kind==dpo::SerialKind::Cancel) &&
        c->barriers.cancel(e.slot.order,e.slot.generation)) {
      if(c->roi) ++c->n.canceled;
      event(c,p,"cancel",e.slot.order,e.slot.unique,e.slot.generation);
    }
  }
}
}

void dpo_init(void) {
  ASSERTM(0, DPE_ORACLE_MODE <= 2, "dpe_oracle_mode must be 0, 1, or 2");
  ASSERTM(0, !DPE_ORACLE_SERIAL || DPE_ORACLE_MODE, "serial oracle requires mode 1 or 2");
  ASSERTM(0, !DPE_ORACLE_SERIAL || (!LOOKAHEAD_BUF_SIZE && !MAP_LOOKAHEAD),
          "serial oracle currently requires lookahead_buf_size=0 and map_lookahead=0");
  unsigned tests = DPE_ORACLE_SELFTEST ? dpo::selftest() + (DPE_ORACLE_SERIAL ? dpo::serial_selftest() + dpo_ft_selftest() : 0) : 0;
  if (tests) fprintf(mystdout, "DPO selftests passed: %u\n", tests);
  cores.clear();
  if (!DPE_ORACLE_MODE) return;
  ASSERTM(0, !PERFECT_BP, "DPO requires the realistic predictor: perfect_bp must be 0");
  ASSERTM(0, FRONTEND == FE_MEMTRACE && NUM_BPS == 1 &&
          BP_MECH == TAGE64K_BP && BP_MECH_L0 == BIMODAL_BP && bp_l0_enabled(),
          "DPO v1 requires memtrace, one primary frontend, main TAGE64K and L0 bimodal");
  ASSERTM(0, !WARMUP, "DPO uses cycle-accurate full_warmup; set warmup=0");
  for (uns p=0; p<NUM_CORES; ++p) {
    cores.emplace_back(new Core);
    auto* c=cores.back().get();
    c->roi=FULL_WARMUP==0;
    c->instructions=inst_count[p];
    c->tests=tests;
    if (DPE_ORACLE_DEBUG_EVENTS) {
      std::string path=std::string(OUTPUT_DIR)+"/dpo.events."+std::to_string(p)+".jsonl";
      c->events=fopen(path.c_str(),"w");
      ASSERTM(p,c->events,"Cannot open DPO debug events");
      event(c,p,"roi_start",0,0,0);
    }
  }
}

Flag dpo_predict(Op* op, Flag realistic) {
  auto* c=state(op->proc_id);
  if (!c || op->off_path || op->uop->cf_type!=CF_CBR) return realistic;
  // One on-path main lookup per dynamic op; record independently of pred_orig.
  ASSERTM(op->proc_id,!op->dpo_pred_valid,"DPO repeated main prediction for a live dynamic op");
  op->dpo_pred_valid=TRUE;
  op->dpo_realistic_dir=realistic;
  op->dpo_oracle_dir=op->oracle_info.dir;
  op->dpo_applied_dir=op->oracle_info.dir;
  if (c->roi) ++c->n.compared;
  if (realistic!=op->oracle_info.dir) {
    op->dpo_generation=c->barriers.add(op->op_num,op->unique_num,cycle_count);
    if (c->roi) { ++c->n.mismatch; ++c->n.created; }
  }
  // value: raw[0], oracle[1], applied[2], L0 raw[3], L0 recovery[4], BTB no_target[5].
  event(c,op->proc_id,"predict",op->op_num,op->unique_num,op->dpo_generation,
        realistic | (op->oracle_info.dir<<1) | (op->dpo_applied_dir<<2) |
        (op->bp_pred_l0.pred_orig<<3) |
        ((op->bp_pred_l0.recovery_point!=RECOVER_AT_NONE)<<4) |
        (op->btb_pred_info->no_target<<5),op->inst->addr);
  if (op->dpo_generation)
    event(c,op->proc_id,"create",op->op_num,op->unique_num,op->dpo_generation,0,op->inst->addr);
  if (DPE_ORACLE_SERIAL && op->dpo_generation) {
    serial_events(c,op->proc_id,c->serial.add(op->op_num,op->dpo_generation,op->unique_num));
    op->dpo_serial_boundary=c->serial.stalled();
  }
  return op->dpo_applied_dir;
}

Flag dpo_frontend_stalled(uns p) {
  auto* c=state(p);
  return DPE_ORACLE_SERIAL && c && c->serial.stalled();
}
void dpo_frontend_check(Op* op) {
  auto* c=state(op->proc_id);
  if (!DPE_ORACLE_SERIAL || !c) return;
  ASSERTM(op->proc_id,!c->serial.stalled() || op->op_num<=c->serial.pending().order,
          "DPO frontend crossed pending branch");
  event(c,op->proc_id,"serial_admit",op->op_num,op->unique_num,op->dpo_generation,
        c->serial.pending().order,op->inst->addr);
}

Flag dpo_gate(Op* op) {
  auto* c=state(op->proc_id);
  if (!c || DPE_ORACLE_MODE!=1 || !c->barriers.blocks(op->op_num)) return FALSE;
  ++c->gated_this_cycle;
  if (c->roi) {
    ++c->n.gated_ops;
    if (!op->dpo_gate_logged) {
      event(c,op->proc_id,"gate",op->op_num,op->unique_num,op->dpo_generation,
            c->barriers.oldest());
      op->dpo_gate_logged=TRUE;
    }
  }
  return TRUE;
}

void dpo_issue(Op* op) {
  auto* c=state(op->proc_id);
  if (!c) return;
  bool younger=c->barriers.blocks(op->op_num);
  ASSERTM(op->proc_id,DPE_ORACLE_MODE!=1 || !younger,"DPO younger op crossed an unresolved barrier");
  if (c->roi && younger) {
    ++c->n.issued_younger;
    if (op->uop->mem_type==MEM_LD) ++c->n.issued_loads;
  }
  event(c,op->proc_id,"issue",op->op_num,op->unique_num,op->dpo_generation,c->barriers.oldest());
}

void dpo_resolve(Op* op) {
  auto* c=state(op->proc_id);
  if (!c || !op->dpo_generation) return;
  const Counter completion=op_get_exec_cycle(op);
  ASSERT(op->proc_id,completion!=MAX_CTR && completion>=cycle_count);
  if (c->barriers.schedule(op->op_num,op->dpo_generation,completion))
    event(c,op->proc_id,"schedule_resolve",op->op_num,op->unique_num,op->dpo_generation,completion);
}

void dpo_advance(uns p) {
  auto* c=state(p);
  if (!c) return;
  ASSERT(p,c->last_advance!=cycle_count);
  c->last_advance=cycle_count;
  c->gated_this_cycle=0;
  for (const auto& b:c->barriers.advance(cycle_count)) {
    if (DPE_ORACLE_SERIAL) c->serial.complete(b.order,b.generation);
    if (c->roi) ++c->n.resolved;
    event(c,p,"resolve",b.order,b.unique,b.generation,b.complete);
  }
  if (DPE_ORACLE_SERIAL) serial_events(c,p,c->serial.advance());
}

void dpo_end_cycle(uns p) {
  auto* c=state(p);
  if (!c || !c->roi) return;
  if (DPE_ORACLE_SERIAL && c->serial.stalled()) {
    ++c->serial_counts[7];
    event(c,p,"serial_stall_cycle",c->serial.active().order,0,c->serial.active().generation,c->serial.pending().order);
  }
  if (!c->barriers.live().empty()) ++c->n.active_cycles;
  if (c->gated_this_cycle) ++c->n.gate_cycles;
  // Sparse per-cycle summaries allow exact debug/stat reconciliation.
  if (!c->barriers.live().empty() || c->gated_this_cycle)
    event(c,p,"cycle",0,0,c->barriers.live().size(),c->gated_this_cycle);
}

void dpo_release(Op* op) {
  auto* c=state(op->proc_id);
  if (!c || !op->dpo_generation) return;
  if (DPE_ORACLE_SERIAL) {
    if (c->serial.active().matches(op->op_num,op->dpo_generation) ||
        c->serial.pending().matches(op->op_num,op->dpo_generation))
      ASSERTM(op->proc_id,op_get_retire_cycle(op)==MAX_CTR,"DPO live serial slot reached retirement");
    serial_events(c,op->proc_id,c->serial.cancel(op->op_num,op->dpo_generation));
  }
  if (c->barriers.find(op->op_num,op->dpo_generation)) {
    ASSERTM(op->proc_id,op_get_retire_cycle(op)==MAX_CTR,"DPO unresolved barrier reached retirement");
    c->barriers.cancel(op->op_num,op->dpo_generation);
    if (c->roi) ++c->n.canceled;
    event(c,op->proc_id,"cancel",op->op_num,op->unique_num,op->dpo_generation);
  }
}

void dpo_start_roi(uns p) {
  auto* c=state(p);
  if (!c || c->roi) return;
  c->roi=true;
  c->n=Counts{};
  for(auto& n:c->serial_counts) n=0;
  c->serial_counts[8]=bool(c->serial.active());
  c->serial_counts[9]=bool(c->serial.pending());
  c->instructions=inst_count[p];
  c->n.carry=c->barriers.live().size();
  event(c,p,"roi_start",0,0,0,c->n.carry);
  if (DPE_ORACLE_SERIAL) {
    const auto& a=c->serial.active(); const auto& b=c->serial.pending();
    event(c,p,"serial_carry_active",a.order,a.unique,a.generation,a.resolved);
    event(c,p,"serial_carry_pending",b.order,b.unique,b.generation,b.resolved);
  }
  for (const auto& entry:c->barriers.live()) {
    const auto& b=entry.second;
    event(c,p,"carry",b.order,b.unique,b.generation,b.complete);
  }
}

void dpo_finish(uns p) {
  auto* c=state(p);
  if (!c) return;
  const auto& n=c->n;
  if (c->roi)
    ASSERT(p,n.carry+n.created==n.resolved+n.canceled+c->barriers.live().size());
  const Counter values[]={
    n.compared,n.mismatch,n.created,n.resolved,n.canceled,n.carry,
    c->roi?c->barriers.live().size():0,n.active_cycles,n.gate_cycles,n.gated_ops,
    n.issued_younger,n.issued_loads,c->roi?inst_count[p]-c->instructions:0,c->tests
  };
  for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i)
    INC_STAT_EVENT(p,DPO_CBR_COMPARED+i,values[i]);
  if (DPE_ORACLE_SERIAL) {
    auto* s=c->serial_counts;
    s[10]=bool(c->serial.active()); s[11]=bool(c->serial.pending());
    ASSERT(p,s[8]+s[0]==s[2]+s[3]+s[10]);
    ASSERT(p,s[9]+s[1]==s[5]+s[4]+s[11]);
    for(unsigned i=0;i<12;++i) INC_STAT_EVENT(p,DPO_SERIAL_STARTED+i,s[i]);
  }
  event(c,p,"finish",0,0,c->barriers.live().size(),inst_count[p]-c->instructions);
  if (c->events) { fclose(c->events); c->events=nullptr; }
  c->finished=true;
}
