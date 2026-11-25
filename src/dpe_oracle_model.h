#ifndef DPE_ORACLE_MODEL_H
#define DPE_ORACLE_MODEL_H
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <stdexcept>
#include <vector>

namespace dpo {
using Num = uint64_t;
constexpr Num never = std::numeric_limits<Num>::max();
inline void check(bool ok, const char* message) {
  if (!ok) throw std::logic_error(message);
}
struct Barrier {
  Num order, generation, unique, predicted, complete = never;
};
struct Completion {
  Num cycle, order, generation;
  bool operator>(const Completion& b) const {
    if (cycle != b.cycle) return cycle > b.cycle;
    return generation > b.generation;
  }
};
// op_num orders barriers. A controller-owned generation protects identity;
// neither a recycled Op pointer nor Scarab's unique_num is used as a key.
class Barriers {
  Num next_generation = 1;
  std::map<Num, Barrier> live_;
  std::priority_queue<Completion, std::vector<Completion>, std::greater<Completion>> completions;
 public:
  const std::map<Num, Barrier>& live() const { return live_; }
  Num oldest() const { return live_.empty() ? never : live_.begin()->first; }
  bool blocks(Num order) const { return oldest() < order; }
  const Barrier* find(Num order, Num generation) const {
    auto it = live_.find(order);
    return it != live_.end() && it->second.generation == generation ? &it->second : nullptr;
  }
  Num add(Num order, Num unique, Num now) {
    check(live_.count(order) == 0, "DPO duplicate live op_num");
    check(next_generation != never, "DPO generation overflow");
    Num g = next_generation++;
    live_.emplace(order, Barrier{order, g, unique, now, never});
    return g;
  }
  bool schedule(Num order, Num generation, Num cycle) {
    auto it = live_.find(order);
    if (it == live_.end() || it->second.generation != generation) return false;
    if (it->second.complete != never) {
      check(it->second.complete == cycle, "DPO conflicting completion");
      return false;
    }
    check(cycle >= it->second.predicted, "DPO completion before prediction");
    it->second.complete = cycle;
    completions.push({cycle, order, generation});
    return true;
  }
  bool cancel(Num order, Num generation) {
    if (!find(order, generation)) return false;
    live_.erase(order);
    return true;
  }
  std::vector<Barrier> advance(Num now) {
    std::vector<Barrier> done;
    while (!completions.empty() && completions.top().cycle <= now) {
      auto e = completions.top();
      completions.pop();
      auto it = live_.find(e.order);
      if (it == live_.end() || it->second.generation != e.generation) continue;
      check(it->second.complete == e.cycle, "DPO stale completion cycle");
      done.push_back(it->second);
      live_.erase(it);
    }
    return done;
  }
};

inline unsigned selftest() {
  unsigned n = 0;
  auto test = [&](bool ok) { check(ok, "DPO selftest failed"); ++n; };
  Barriers b;
  test(!b.blocks(999));
  Num x = b.add(10, 77, 2);
  test(!b.blocks(9) && !b.blocks(10) && b.blocks(11));
  Num y = b.add(20, 78, 3);
  test(b.blocks(20) && b.live().size() == 2); // B2 waits behind B1.
  test(b.schedule(10,x,12));
  test(!b.schedule(10,x,12)); // duplicate callback is harmless.
  test(b.advance(11).empty() && b.blocks(11));
  test(b.advance(12).size() == 1 && !b.blocks(20) && b.blocks(21));
  test(b.schedule(20,y,15) && b.advance(15).size() == 1 && !b.blocks(21));
  Num old = b.add(30, 7, 20);
  test(b.schedule(30,old,30) && b.cancel(30,old));
  Num fresh = b.add(30, 7, 22); // deliberately reuse BOTH op_num and unique_num.
  test(fresh != old && !b.cancel(30,old) && !b.schedule(30,old,25));
  test(b.advance(30).empty() && b.find(30,fresh));
  test(b.schedule(30,fresh,40) && b.advance(40).size() == 1);
  Num a=b.add(40,1,41), c=b.add(50,2,41);
  b.schedule(50,c,42); b.schedule(40,a,45); // Full may resolve younger first.
  test(b.advance(42).size() == 1 && b.oldest() == 40);
  test(b.advance(45).size() == 1 && b.live().empty());
  a=b.add(60,1,46); c=b.add(70,2,46);
  b.schedule(60,a,50); b.schedule(70,c,50);
  test(b.advance(50).size() == 2 && b.live().empty());
  a=b.add(80,1,51); c=b.add(90,2,51);
  b.schedule(90,c,60); test(b.cancel(90,c) && b.oldest()==80);
  test(b.advance(60).empty() && b.find(80,a)); // squash younger only.
  Barriers other;
  Num o=other.add(80,1,52);
  test(b.cancel(80,a) && other.find(80,o)); // per-core isolation.
  test(other.live().size()==1); // ROI/end observation must not clear state.
  bool duplicate=false;
  try { other.add(80,1,53); } catch (const std::logic_error&) { duplicate=true; }
  test(duplicate);
  return n;
}
}
#endif
