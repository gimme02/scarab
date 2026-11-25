#ifndef DPE_SERIAL_MODEL_H
#define DPE_SERIAL_MODEL_H
#include "dpe_oracle_model.h"

namespace dpo {
struct SerialSlot {
  Num order=never, generation=0, unique=0;
  bool resolved=false;
  explicit operator bool() const { return generation!=0; }
  bool matches(Num o, Num g) const { return generation && order==o && generation==g; }
};
enum class SerialKind { Start, Stop, End, Cancel, PendingCancel, Handoff, HandoffResolved };
struct SerialEvent { SerialKind kind; SerialSlot slot; Num other=never; };
class Serial {
  SerialSlot active_, pending_;
 public:
  const SerialSlot& active() const { return active_; }
  const SerialSlot& pending() const { return pending_; }
  bool stalled() const { return bool(pending_); }
  std::vector<SerialEvent> add(Num order, Num generation, Num unique) {
    check(!pending_, "DPO prediction crossed serial stop");
    SerialSlot s{order,generation,unique,false};
    check(generation!=0, "DPO serial generation is zero");
    if (!active_) { active_=s; return {{SerialKind::Start,s}}; }
    check(active_.order<order, "DPO serial order inversion");
    pending_=s;
    return {{SerialKind::Stop,s,active_.order}};
  }
  void complete(Num order, Num generation) {
    if (active_.matches(order,generation)) active_.resolved=true;
    if (pending_.matches(order,generation)) pending_.resolved=true;
  }
  // Called only after ALL same-cycle completions have been marked.
  std::vector<SerialEvent> advance() {
    std::vector<SerialEvent> out;
    if (!active_ || !active_.resolved) return out;
    out.push_back({SerialKind::End,active_});
    active_={};
    if (pending_) {
      active_=pending_; pending_={};
      out.push_back({SerialKind::Handoff,active_});
      out.push_back({SerialKind::Start,active_});
      if (active_.resolved) {
        out.push_back({SerialKind::HandoffResolved,active_});
        out.push_back({SerialKind::End,active_});
        active_={};
      }
    }
    return out;
  }
  std::vector<SerialEvent> cancel(Num order, Num generation) {
    std::vector<SerialEvent> out;
    if (pending_.matches(order,generation) || active_.matches(order,generation)) {
      if (pending_) { out.push_back({SerialKind::PendingCancel,pending_}); pending_={}; }
      if (active_.matches(order,generation)) {
        out.push_back({SerialKind::Cancel,active_}); active_={};
      }
    }
    return out;
  }
};
inline unsigned serial_selftest() {
  unsigned n=0;
  auto test=[&](bool ok){ check(ok,"DPO serial selftest failed"); ++n; };
  Serial s;
  test(!s.active() && !s.pending() && !s.stalled());
  test(s.add(10,1,7)[0].kind==SerialKind::Start);
  test(s.add(20,2,8)[0].kind==SerialKind::Stop && s.stalled());
  bool rejected=false;
  try { s.add(30,3,9); } catch(const std::logic_error&) { rejected=true; }
  test(rejected && s.pending().order==20);
  s.complete(20,2); test(s.advance().empty() && s.stalled());
  s.complete(10,1); auto e=s.advance();
  test(e.size()==5 && e[3].kind==SerialKind::HandoffResolved);
  test(!s.active() && !s.pending());
  s.add(10,3,7); s.add(20,4,8);
  s.complete(10,1); test(s.advance().empty()); // reused order, stale generation
  test(s.cancel(20,2).empty() && s.stalled());
  s.complete(10,3); e=s.advance();
  test(e.size()==3 && s.active().order==20 && !s.stalled());
  s.add(30,5,9); test(s.stalled());
  s.complete(20,4); s.complete(30,5); e=s.advance();
  test(e.size()==5 && !s.active()); // simultaneous completion
  s.add(40,6,10); s.add(50,7,11);
  test(s.cancel(50,7).size()==1 && !s.stalled() && s.active().order==40);
  s.add(60,8,12); e=s.cancel(40,6);
  test(e.size()==2 && !s.active() && !s.pending());
  s.complete(60,8); test(s.advance().empty());
  s.add(70,9,13); s.add(80,10,14); s.complete(80,10);
  test(s.cancel(80,10).size()==1 && s.active().order==70);
  Serial other; other.add(70,11,13);
  test(s.cancel(70,9).size()==1 && bool(other.active()));
  test(other.active().generation==11); // warmup/ROI observation retains state
  other.complete(70,11); test(other.advance().size()==1 && !other.active());
  return n;
}
}
#endif
