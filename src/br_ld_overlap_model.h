#ifndef BR_LD_OVERLAP_MODEL_H
#define BR_LD_OVERLAP_MODEL_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

// Observer-only state. No Scarab objects, scheduling decisions, or memory requests
// are retained here; dynamic IDs and immutable event timestamps are sufficient.
namespace brld {
using Tick = uint64_t;
using Id = uint64_t;
constexpr Tick never = std::numeric_limits<Tick>::max();
constexpr unsigned groups = 7;
inline void check(bool ok, const char* message) {
  if (!ok) throw std::logic_error(message);
}
inline bool target(bool conditional, bool exec_recovery, bool wrong_direction, bool offpath) {
  return conditional && exec_recovery && wrong_direction && !offpath;
}
using Interval = std::pair<Tick, Tick>;
inline Tick covered(std::vector<Interval> intervals, Tick lo, Tick hi) {
  if (hi <= lo) return 0;
  std::sort(intervals.begin(), intervals.end());
  Tick total = 0, end = lo;
  for (auto range : intervals) {
    Tick a = std::max(lo, range.first), b = std::min(hi, range.second);
    if (b > std::max(a, end)) total += b - std::max(a, end);
    end = std::max(end, b);
  }
  return total;
}
struct Load {
  Id id = 0;
  Tick order = 0, issue = never, ready = never;
  bool llc = false, merged_llc = false, canceled = false;
};
struct Node {
  std::vector<Id> ancestors;
  bool known = true;
};
struct Candidate {
  std::shared_ptr<Load> load;
  bool dependent = false;
};
struct Window {
  Id id = 0;
  Tick order = 0, pc = 0, dispatch = never, resolve = never, recover = never, resume = never;
  bool known = true;
  std::vector<Candidate> candidates;
};
struct Group {
  Tick overlap = 0, at_resolve = 0;
  Tick cycles = 0, pre_cycles = 0, post_cycles = 0;
  // none, dependent only, independent only, both
  std::array<Tick, 4> dependency{{0, 0, 0, 0}};
  // none, partial, full (zero length is classified none)
  std::array<Tick, 3> recovery{{0, 0, 0}};
};
struct Counts {
  Tick total = 0, complete = 0, censored = 0, canceled = 0, unknown_sources = 0;
  Tick boundary_recoveries = 0, unrecovered = 0;
  Tick cycles = 0, pre_cycles = 0, post_cycles = 0;
  std::array<Group, groups> group{};
};
inline bool qualifies(const Load& l, unsigned group) {
  static const Tick thresholds[3] = {50, 100, 200};
  if (l.canceled || l.issue == never || l.ready == never) return false;
  if (group < 3) return l.ready - l.issue >= thresholds[group];
  if (group == 3) return l.llc;
  return l.llc && l.ready - l.issue >= thresholds[group - 4];
}
class Observer {
 public:
  using Report = void (*)(void*, const Window&, bool);
  explicit Observer(bool in_roi = false) : roi(in_roi) {}
  bool roi = false, finished = false;
  Counts count;
  Report report = nullptr;
  void* context = nullptr;

  void mapped(Id id, Tick order, bool is_load, const std::vector<Id>& sources, Tick now) {
    check(!nodes.count(id), "BRLD duplicate live dynamic ID");
    Node node;
    for (Id source : sources) {
      auto it = nodes.find(source);
      if (it == nodes.end()) {
        node.known = false;
        if (roi) ++count.unknown_sources;
        continue;
      }
      node.known = node.known && it->second.known;
      for (Id ancestor : it->second.ancestors) {
        auto l = loads.find(ancestor);
        if (l != loads.end() && !l->second->canceled && l->second->ready > now)
          node.ancestors.push_back(ancestor);
      }
    }
    if (is_load) {
      auto load = std::make_shared<Load>();
      load->id = id;
      load->order = order;
      loads.emplace(id, load);
      node.ancestors.push_back(id);
    }
    std::sort(node.ancestors.begin(), node.ancestors.end());
    node.ancestors.erase(std::unique(node.ancestors.begin(), node.ancestors.end()), node.ancestors.end());
    nodes.emplace(id, std::move(node));
  }

  void dispatch(Id id, Tick order, Tick pc, bool is_target, Tick now) {
    // Called only for correct-path operations. Older instructions still flowing
    // through the pipeline cannot close a recovery window.
    for (auto& entry : windows) {
      Window& w = entry.second;
      if (w.recover != never && w.resume == never && order > w.order && now >= w.recover)
        w.resume = now;
    }
    service(now);
    if (!roi || !is_target) return;
    check(!windows.count(id), "BRLD duplicate branch dispatch");
    Window w;
    w.id = id; w.order = order; w.pc = pc; w.dispatch = now;
    auto node = nodes.find(id);
    w.known = node != nodes.end() && node->second.known;
    for (const auto& entry : loads) {
      const auto& load = entry.second;
      if (load->order >= order || load->ready <= now || load->canceled) continue;
      bool dependent = node != nodes.end() &&
          std::binary_search(node->second.ancestors.begin(), node->second.ancestors.end(), load->id);
      w.candidates.push_back({load, dependent});
    }
    // Deterministic logs and iteration even when unordered_map order changes.
    std::sort(w.candidates.begin(), w.candidates.end(), [](const Candidate& a, const Candidate& b) {
      return a.load->id < b.load->id;
    });
    windows.emplace(id, std::move(w));
    ++count.total;
  }
  void issue(Id id, Tick now) {
    auto it = loads.find(id);
    if (it == loads.end()) return;
    // Retried attempts retain the first actual issue: queueing/retry time is
    // part of observed load latency, and must not produce a second load.
    if (it->second->issue == never) it->second->issue = now;
  }
  void ready(Id id, Tick result_cycle, Tick now) {
    auto it = loads.find(id);
    if (it == loads.end()) return;
    auto& l = *it->second;
    check(l.issue != never && result_cycle >= l.issue, "BRLD load completion before issue");
    check(l.ready == never || l.ready == result_cycle, "BRLD inconsistent result-ready cycle");
    l.ready = result_cycle;
    service(now);
  }
  void llc_miss(Id id, bool merged) {
    auto it = loads.find(id);
    if (it != loads.end()) {
      it->second->llc = true;
      it->second->merged_llc |= merged;
    }
  }
  void resolve(Id id, Tick when) {
    auto it = windows.find(id);
    if (it != windows.end()) {
      check(when >= it->second.dispatch, "BRLD resolution before dispatch");
      it->second.resolve = when;
    }
  }
  void recover(Id id, Tick now, bool eligible) {
    auto it = windows.find(id);
    if (it != windows.end()) {
      check(it->second.resolve != never && now >= it->second.resolve, "BRLD invalid recovery time");
      check(it->second.recover == never, "BRLD repeated recovery");
      it->second.recover = now;
    } else if (roi && eligible) {
      ++count.boundary_recoveries;
    }
  }
  void release(Id id, bool retired, Tick now) {
    auto it = loads.find(id);
    if (it != loads.end()) {
      if (!retired) it->second->canceled = true;
      loads.erase(it);
    }
    auto w = windows.find(id);
    if (!retired && w != windows.end()) {
      --count.total;
      ++count.canceled;
      windows.erase(w);
    }
    nodes.erase(id);
    service(now);
  }
  void start_roi() {
    // Warmup windows were never enrolled; retain live loads and ancestors.
    roi = true;
  }
  void finish(Tick now) {
    if (finished) return;
    service(now);
    count.censored = windows.size();
    for (const auto& entry : windows) {
      if (entry.second.recover == never) ++count.unrecovered;
      if (report) report(context, entry.second, false);
    }
    check(count.total == count.complete + count.censored, "BRLD denominator mismatch");
    finished = true;
  }

 private:
  std::unordered_map<Id, Node> nodes;
  std::unordered_map<Id, std::shared_ptr<Load>> loads;
  std::map<Id, Window> windows;

  bool observable(const Window& w, Tick now) const {
    if (!w.known || w.resolve == never || w.recover == never || w.resume == never || w.resume > now)
      return false;
    for (const auto& c : w.candidates) {
      const auto& l = *c.load;
      if (l.canceled || l.issue >= w.resume) continue; // includes never-issued
      if (l.ready == never || l.ready > now) return false;
    }
    return true;
  }
  void service(Tick now) {
    for (auto it = windows.begin(); it != windows.end();) {
      if (!observable(it->second, now)) { ++it; continue; }
      summarize(it->second);
      if (report) report(context, it->second, true);
      it = windows.erase(it);
    }
  }
  void summarize(const Window& w) {
    check(w.dispatch <= w.resolve && w.resolve <= w.recover && w.recover <= w.resume,
          "BRLD non-monotonic branch timestamps");
    ++count.complete;
    count.cycles += w.resume - w.dispatch;
    count.pre_cycles += w.resolve - w.dispatch;
    count.post_cycles += w.resume - w.resolve;
    for (unsigned g = 0; g < groups; ++g) {
      std::vector<Interval> independent;
      bool dep = false, at_resolve = false;
      for (const auto& c : w.candidates) {
        const auto& l = *c.load;
        if (!qualifies(l, g) || l.issue >= w.resume || l.ready <= w.dispatch) continue;
        if (c.dependent) dep = true;
        else {
          independent.push_back({l.issue, l.ready});
          at_resolve |= l.issue <= w.resolve && w.resolve < l.ready;
        }
      }
      Tick all = covered(independent, w.dispatch, w.resume);
      Tick pre = covered(independent, w.dispatch, w.resolve);
      Tick post = covered(independent, w.resolve, w.resume);
      check(all == pre + post && all <= w.resume - w.dispatch, "BRLD invalid union coverage");
      auto& out = count.group[g];
      out.overlap += all > 0;
      out.at_resolve += at_resolve;
      out.cycles += all; out.pre_cycles += pre; out.post_cycles += post;
      ++out.dependency[(dep ? 1 : 0) + (all ? 2 : 0)];
      ++out.recovery[post == 0 ? 0 : post == w.resume - w.resolve ? 2 : 1];
    }
  }
};

// Tests exercise the same state machine used by Scarab, through SCI's normal
// build/run path. No separate simulator or direct compiler invocation is needed.
inline unsigned selftest() {
  unsigned tests = 0;
  check(covered({{0, 100}, {20, 140}, {140, 160}}, 30, 150) == 120, "union/adjacency"); ++tests;
  check(covered({{0, 30}, {60, 90}}, 30, 60) == 0, "half-open boundary"); ++tests;
  check(target(true,true,true,false) && !target(true,false,true,false) &&
        !target(false,true,true,false) && !target(true,true,false,false) &&
        !target(true,true,true,true), "branch filter"); ++tests;
  auto branch = [](Observer& o, Id id, Tick order, const std::vector<Id>& sources, Tick d, Tick r, Tick resume) {
    o.mapped(id, order, false, sources, d);
    o.dispatch(id, order, 4096, true, d);
    o.resolve(id, r); o.recover(id, r+1, true);
    o.dispatch(id+100, order+1, 4100, false, resume);
  };
  {
    Observer o(true); branch(o,2,2,{},20,30,40); o.finish(50);
    check(o.count.total==1 && o.count.complete==1 && o.count.group[1].dependency[0]==1, "no load"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0); o.issue(1,0); o.llc_miss(1,false);
    branch(o,2,2,{},20,30,40); o.ready(1,150,150); o.finish(150);
    check(o.count.group[1].overlap==1 && o.count.group[1].cycles==20 &&
          o.count.group[1].at_resolve==1 && o.count.group[1].recovery[2]==1 &&
          o.count.group[2].overlap==0 && o.count.group[5].overlap==1, "full independent / thresholds"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0); o.issue(1,0);
    o.mapped(2,2,false,{1},10);
    branch(o,3,3,{2},20,160,180); o.ready(1,150,150); o.finish(200);
    check(o.count.group[1].dependency[1]==1 && o.count.group[1].overlap==0, "transitive dependency"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0); o.issue(1,0);
    branch(o,2,2,{1},20,160,180); o.ready(1,150,150); o.finish(200);
    check(o.count.group[1].dependency[1]==1, "direct dependency"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0); o.issue(1,0);
    o.mapped(2,2,true,{},5); o.issue(2,5); o.llc_miss(2,true);
    branch(o,3,3,{1},20,160,180); o.ready(1,150,150); o.ready(2,170,180); o.finish(200);
    check(o.count.group[1].dependency[3]==1 && o.count.group[1].cycles==150 &&
          o.count.group[1].recovery[1]==1 && o.count.group[3].overlap==1, "mixed and merged miss"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0); o.mapped(2,2,true,{},0);
    o.issue(1,0); o.issue(2,10);
    branch(o,3,3,{},20,80,100); o.ready(1,200,200); o.ready(2,210,210); o.finish(220);
    check(o.count.group[1].overlap==1 && o.count.group[1].cycles==80, "overlapping loads count once"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0);
    branch(o,2,2,{},20,30,40); o.finish(40);
    check(o.count.complete==1 && o.count.group[1].overlap==0, "unissued-at-resume excluded"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0); o.mapped(2,2,false,{},20);
    o.dispatch(2,2,4096,true,20); o.issue(1,25); o.issue(1,27);
    o.resolve(2,30); o.recover(2,31,true); o.dispatch(3,3,4100,false,40);
    o.ready(1,125,125); o.finish(125);
    check(o.count.group[1].cycles==15 && o.count.group[1].at_resolve==1, "late issue / retry"); ++tests;
  }
  {
    Observer o(false); o.mapped(1,1,true,{},0); o.issue(1,0); o.start_roi();
    branch(o,2,2,{},20,30,40); o.ready(1,100,100); o.finish(100);
    check(o.count.group[1].overlap==1, "warmup-carried load"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0); o.issue(1,0);
    branch(o,2,2,{},20,30,40); o.finish(50);
    check(o.count.censored==1 && o.count.complete==0, "unfinished load censored"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0); o.issue(1,0);
    o.release(1,false,10); o.mapped(9,1,true,{},11); o.issue(9,11);
    branch(o,2,2,{},20,30,40); o.ready(9,150,150); o.finish(150);
    check(o.count.group[1].overlap==1 && o.count.total==1, "squash / recycled op slot"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,false,{},0); o.dispatch(1,1,4096,true,0);
    o.release(1,false,1); o.finish(2);
    check(o.count.canceled==1 && o.count.total==0, "canceled branch"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0); o.issue(1,0); o.ready(1,100,90);
    branch(o,2,2,{},100,110,120); o.finish(120);
    check(o.count.group[1].overlap==0, "ready at dispatch is no overlap"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,false,{99},0); o.dispatch(1,1,4096,true,1);
    o.resolve(1,2); o.recover(1,3,true); o.dispatch(2,2,4100,false,4); o.finish(5);
    check(o.count.unknown_sources==1 && o.count.censored==1, "unknown ancestry never independent"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0); o.issue(1,0);
    o.mapped(2,2,false,{},50); o.dispatch(2,2,4096,true,50);
    o.ready(1,100,90); o.resolve(2,100); o.recover(2,101,true);
    o.dispatch(3,3,4100,false,120); o.finish(120);
    check(o.count.group[1].overlap==1 && o.count.group[1].at_resolve==0 &&
          o.count.group[1].post_cycles==0, "ready at resolve is not outstanding"); ++tests;
  }
  {
    Observer o(true); o.mapped(1,1,true,{},0); o.issue(1,0); o.llc_miss(1,true);
    o.mapped(2,2,false,{},2); o.dispatch(2,2,4096,true,2);
    o.ready(1,10,5); o.resolve(2,6); o.recover(2,7,true);
    o.dispatch(99,1,4000,false,8); o.dispatch(3,3,4100,false,15); o.finish(15);
    check(o.count.group[3].cycles==8 && o.count.group[0].overlap==0 &&
          o.count.post_cycles==9, "fast LLC miss / older dispatch does not resume"); ++tests;
  }
  return tests;
}
} // namespace brld
#endif
