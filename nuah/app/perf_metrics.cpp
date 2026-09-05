#include "nuah/perf_metrics.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
namespace {
struct Aggregate { uint64_t calls = 0, samples = 0, total_ns = 0, max_ns = 0, slow = 0; };
struct State { std::mutex mutex; Aggregate input; std::array<Aggregate, 3> jni{}; uint64_t next_report_ns = 0; };
struct Report { bool ready = false; Aggregate input; std::array<Aggregate, 3> jni{}; };
State& state() { static State value; return value; }
uint64_t now_ns() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
bool enabled() { static const bool v = [] { const char* r = std::getenv("NUAH_PERF_TRACE"); return r && *r && std::strcmp(r, "0") != 0; }(); return v; }
const char* jni_name(std::size_t i) { static constexpr const char* kNames[] = {"key", "pointer", "other"}; return i < 3 ? kNames[i] : "other"; }
std::size_t jni_index(const char* kind) {
  for (std::size_t i = 0; i < 2; ++i) if (kind && std::strcmp(kind, jni_name(i)) == 0) return i;
  return 2;
}
void add(Aggregate& a, uint64_t ns, uint64_t samples) { ++a.calls; a.samples += samples; a.total_ns += ns; a.max_ns = std::max(a.max_ns, ns); if (ns > 16666667ULL) ++a.slow; }
void print_aggregate(const char* name, const Aggregate& a) {
  if (!a.calls) return;
  std::fprintf(stderr, "nuah perf: %s calls=%llu samples=%llu avg_us=%llu max_us=%llu over16ms=%llu\n", name, (unsigned long long)a.calls, (unsigned long long)a.samples, (unsigned long long)(a.total_ns / a.calls / 1000ULL), (unsigned long long)(a.max_ns / 1000ULL), (unsigned long long)a.slow);
}
void take_report_if_due(State& v, uint64_t now, Report& r) {
  if (v.next_report_ns == 0) v.next_report_ns = now + 1000000000ULL;
  if (now < v.next_report_ns) return;
  v.next_report_ns = now + 1000000000ULL; r.ready = true; r.input = v.input; r.jni = v.jni; v.input = {}; v.jni = {};
}
void print_report(const Report& r) {
  if (!r.ready) return;
  print_aggregate("input", r.input);
  for (std::size_t i = 0; i < r.jni.size(); ++i) print_aggregate(jni_name(i), r.jni[i]);
}
void record(State& v, std::size_t idx, uint64_t ns, uint64_t samples, bool is_input) {
  Report r;
  { std::scoped_lock lock(v.mutex); add(is_input ? v.input : v.jni[idx], ns, samples); take_report_if_due(v, now_ns(), r); }
  print_report(r);
}
}  // namespace
extern "C" int nuah_perf_trace_enabled(void) { return enabled() ? 1 : 0; }
extern "C" void nuah_perf_record_input(uint64_t ns, unsigned int n) { if (enabled()) record(state(), 0, ns, n, true); }
extern "C" void nuah_perf_record_jni(const char* kind, uint64_t ns, int /*result*/) { if (enabled()) record(state(), jni_index(kind), ns, 1, false); }
