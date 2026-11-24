#include "br_ld_overlap_model.h"
#include <cstdio>
#include <string>

extern "C" {
#include "globals/global_defs.h"
#include "globals/global_vars.h"
#include "core.param.h"
#include "general.param.h"
#include "op.h"
#include "statistics.h"
#include "sim.h"
#include "freq.h"
#include "br_ld_overlap.h"
}

namespace {
struct Core {
  brld::Observer observer;
  FILE* events = nullptr;
  unsigned tests = 0;
  Counter roi_inst_start = 0;
  explicit Core(bool roi) : observer(roi) {}
};
std::vector<std::unique_ptr<Core>> cores;
Core* state(uns proc) {
  return proc < cores.size() && !cores[proc]->observer.finished ? cores[proc].get() : nullptr;
}
bool target_op(const Op* op) {
  return brld::target(op->uop->cf_type == CF_CBR,
                     op->bp_pred_main.recovery_point == RECOVER_AT_EXEC,
                     op->bp_pred_main.pred_orig != op->oracle_info.dir, op->off_path);
}
void report(void* context, const brld::Window& w, bool complete) {
  FILE* f = static_cast<Core*>(context)->events;
  if (!f) return;
  fprintf(f, "{\"branch\":%llu,\"pc\":%llu,\"order\":%llu,\"dispatch\":%llu,\"resolve\":%llu,"
             "\"recover\":%llu,\"resume\":%llu,\"complete\":%s,\"known\":%s,\"loads\":[",
          (unsigned long long)w.id, (unsigned long long)w.pc, (unsigned long long)w.order,
          (unsigned long long)w.dispatch, (unsigned long long)w.resolve, (unsigned long long)w.recover,
          (unsigned long long)w.resume, complete ? "true" : "false", w.known ? "true" : "false");
  bool first = true;
  for (const auto& c : w.candidates) {
    const auto& l = *c.load;
    fprintf(f, "%s{\"id\":%llu,\"order\":%llu,\"issue\":%llu,\"ready\":%llu,\"dependent\":%s,"
               "\"llc\":%s,\"merged_llc\":%s,\"canceled\":%s}", first ? "" : ",",
            (unsigned long long)l.id, (unsigned long long)l.order, (unsigned long long)l.issue,
            (unsigned long long)l.ready, c.dependent ? "true" : "false", l.llc ? "true" : "false",
            l.merged_llc ? "true" : "false", l.canceled ? "true" : "false");
    first = false;
  }
  fprintf(f, "]}\n");
}
}

void brld_init(void) {
  cores.clear();
  unsigned tests = BR_LD_OVERLAP_SELFTEST ? brld::selftest() : 0;
  if (tests) fprintf(mystdout, "BRLD selftests passed: %u\n", tests);
  if (!BR_LD_OVERLAP_ENABLE) return;
  for (uns proc = 0; proc < NUM_CORES; ++proc) {
    cores.emplace_back(new Core(FULL_WARMUP == 0));
    auto& c = *cores.back();
    c.tests = tests;
    if (BR_LD_OVERLAP_DEBUG_EVENTS) {
      std::string path = std::string(OUTPUT_DIR) + "/brld.events." + std::to_string(proc) + ".jsonl";
      c.events = fopen(path.c_str(), "w");
      brld::check(c.events != nullptr, "BRLD could not open debug event output");
      c.observer.report = report;
      c.observer.context = &c;
    }
  }
}
void brld_mapped(Op* op) {
  auto c = state(op->proc_id);
  if (!c || op->off_path) return;
  std::vector<brld::Id> sources;
  for (uns i = 0; i < op->num_srcs; ++i) {
    const Src_Info& src = op->src_info[i];
    // A retired/recycled producer cannot carry an outstanding older load:
    // in-order retirement would have waited for that load first.
    if (src.unique_num && src.op->op_pool_valid && src.op->unique_num == src.unique_num && !src.op->off_path)
      sources.push_back(src.unique_num);
  }
  c->observer.mapped(op->unique_num, op->op_num, op->uop->mem_type == MEM_LD, sources, cycle_count);
}
void brld_dispatch(Op* op) {
  auto c = state(op->proc_id);
  if (c && !op->off_path)
    c->observer.dispatch(op->unique_num, op->op_num, op->inst->addr, target_op(op), cycle_count);
}
void brld_issue(Op* op) {
  auto c = state(op->proc_id);
  if (c && !op->off_path && op->uop->mem_type == MEM_LD) c->observer.issue(op->unique_num, cycle_count);
}
void brld_ready(Op* op) {
  auto c = state(op->proc_id);
  if (c && !op->off_path && op->uop->mem_type == MEM_LD)
    c->observer.ready(op->unique_num, op_get_wake_cycle(op), cycle_count);
}
void brld_llc_miss(Op* op, Flag merged) {
  auto c = state(op->proc_id);
  if (c && !op->off_path && op->uop->mem_type == MEM_LD) c->observer.llc_miss(op->unique_num, merged);
}
void brld_resolve(Op* op) {
  auto c = state(op->proc_id);
  if (c && target_op(op)) c->observer.resolve(op->unique_num, op_get_exec_cycle(op));
}
void brld_recover(Op* op) {
  auto c = state(op->proc_id);
  if (c) c->observer.recover(op->unique_num, cycle_count, target_op(op));
}
void brld_release(Op* op) {
  auto c = state(op->proc_id);
  if (c && !op->off_path)
    c->observer.release(op->unique_num, op_get_retire_cycle(op) != MAX_CTR, cycle_count);
}
void brld_start_roi(uns proc_id) {
  auto c = state(proc_id);
  if (c) {
    c->roi_inst_start = inst_count[proc_id];
    c->observer.start_roi();
  }
}
void brld_finish(uns proc_id) {
  auto c = state(proc_id);
  if (!c) return;
  c->observer.finish(freq_cycle_count(FREQ_DOMAIN_CORES[proc_id]));
  const auto& n = c->observer.count;
  INC_STAT_EVENT(proc_id, BRLD_BRANCHES_TOTAL, n.total);
  INC_STAT_EVENT(proc_id, BRLD_BRANCHES_COMPLETE, n.complete);
  INC_STAT_EVENT(proc_id, BRLD_BRANCHES_CENSORED, n.censored);
  INC_STAT_EVENT(proc_id, BRLD_BRANCHES_CANCELED, n.canceled);
  INC_STAT_EVENT(proc_id, BRLD_UNKNOWN_LIVE_SOURCES, n.unknown_sources);
  INC_STAT_EVENT(proc_id, BRLD_BOUNDARY_RECOVERIES, n.boundary_recoveries);
  INC_STAT_EVENT(proc_id, BRLD_UNRECOVERED_AT_END, n.unrecovered);
  INC_STAT_EVENT(proc_id, BRLD_WINDOW_CYCLES, n.cycles);
  INC_STAT_EVENT(proc_id, BRLD_PRE_CYCLES, n.pre_cycles);
  INC_STAT_EVENT(proc_id, BRLD_POST_CYCLES, n.post_cycles);
  INC_STAT_EVENT(proc_id, BRLD_SELFTESTS_PASSED, c->tests);
  const Stat_Enum bases[brld::groups] = {BRLD_T50_OVERLAP, BRLD_T100_OVERLAP, BRLD_T200_OVERLAP,
      BRLD_LLC_OVERLAP, BRLD_LLC_T50_OVERLAP, BRLD_LLC_T100_OVERLAP, BRLD_LLC_T200_OVERLAP};
  for (unsigned i = 0; i < brld::groups; ++i) {
    const auto& g = n.group[i];
    const brld::Tick values[] = {g.overlap, g.at_resolve, g.cycles, g.pre_cycles, g.post_cycles,
        g.dependency[0], g.dependency[1], g.dependency[2], g.dependency[3],
        g.recovery[0], g.recovery[1], g.recovery[2]};
    for (unsigned j = 0; j < 12; ++j) INC_STAT_EVENT(proc_id, bases[i] + j, values[j]);
  }
  const Counter roi_instructions = inst_count[proc_id] - c->roi_inst_start;
  INC_STAT_EVENT(proc_id, BRLD_ROI_INSTRUCTIONS, roi_instructions);
  const double scale = roi_instructions ? 1e6 / (double)roi_instructions : 0;
  for (unsigned stat = BRLD_BRANCHES_TOTAL; stat <= BRLD_LLC_T200_REC_FULL; ++stat)
    INC_STAT_VALUE(proc_id, BRLD_NORM_BRANCHES_TOTAL + stat - BRLD_BRANCHES_TOTAL,
                   GET_STAT_EVENT(proc_id, stat) * scale);
  if (c->events) { fclose(c->events); c->events = nullptr; }
}
