#include "nuah/perf_metrics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
using Clock = std::chrono::steady_clock;

struct Aggregate {
  uint64_t calls = 0;
  uint64_t samples = 0;
  uint64_t total_ns = 0;
  uint64_t max_ns = 0;
  uint64_t slow = 0;
};

struct State {
  std::mutex mutex;
  Aggregate input;
  std::array<Aggregate, 3> jni{};
  uint64_t next_report_ns = 0;
};

State& state() {
  static State value;
  return value;
}

uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

bool enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_PERF_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

Aggregate& jni_aggregate(const char* kind, State& value) {
  if (kind && std::strcmp(kind, "key") == 0) return value.jni[0];
  if (kind && std::strcmp(kind, "pointer") == 0) return value.jni[1];
  return value.jni[2];
}

const char* jni_name(std::size_t index) {
  switch (index) {
    case 0: return "key";
    case 1: return "pointer";
    default: return "other";
  }
}

void add(Aggregate& aggregate, uint64_t duration_ns, uint64_t samples) {
  ++aggregate.calls;
  aggregate.samples += samples;
  aggregate.total_ns += duration_ns;
  aggregate.max_ns = std::max(aggregate.max_ns, duration_ns);
  /* A 16.67 ms frame budget is a useful, renderer-independent warning line. */
  if (duration_ns > 16666667ULL) ++aggregate.slow;
}

void print_aggregate(const char* name, const Aggregate& aggregate) {
  if (!aggregate.calls) return;
  const uint64_t average_ns = aggregate.total_ns / aggregate.calls;
  std::fprintf(stderr,
               "nuah perf: %s calls=%llu samples=%llu avg_us=%llu max_us=%llu over16ms=%llu\n",
               name, static_cast<unsigned long long>(aggregate.calls),
               static_cast<unsigned long long>(aggregate.samples),
               static_cast<unsigned long long>(average_ns / 1000ULL),
               static_cast<unsigned long long>(aggregate.max_ns / 1000ULL),
               static_cast<unsigned long long>(aggregate.slow));
}

void report_if_due(State& value, uint64_t now) {
  if (value.next_report_ns == 0) value.next_report_ns = now + 1000000000ULL;
  if (now < value.next_report_ns) return;
  value.next_report_ns = now + 1000000000ULL;
  print_aggregate("input", value.input);
  for (std::size_t index = 0; index < value.jni.size(); ++index)
    print_aggregate(jni_name(index), value.jni[index]);
  value.input = {};
  value.jni = {};
}
}  // namespace

extern "C" int nuah_perf_trace_enabled(void) { return enabled() ? 1 : 0; }

extern "C" void nuah_perf_record_input(uint64_t duration_ns,
                                         unsigned int event_count) {
  if (!enabled()) return;
  State& value = state();
  std::scoped_lock lock(value.mutex);
  add(value.input, duration_ns, event_count);
  report_if_due(value, now_ns());
}

extern "C" void nuah_perf_record_jni(const char* kind, uint64_t duration_ns,
                                      int /*result*/) {
  if (!enabled()) return;
  State& value = state();
  std::scoped_lock lock(value.mutex);
  add(jni_aggregate(kind, value), duration_ns, 1);
  report_if_due(value, now_ns());
}
