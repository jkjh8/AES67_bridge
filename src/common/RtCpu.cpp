#include "common/RtCpu.h"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace aes67 {
namespace {

struct RtCores {
  std::vector<ULONG> ids;
  std::vector<int> logical;
};

RtCores Compute() {
  RtCores rc;
  ULONG len = 0;
  GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
  if (!len) return rc;
  std::vector<uint8_t> buf(len);
  auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data());
  if (!GetSystemCpuSetInformation(info, len, &len, GetCurrentProcess(), 0)) return rc;

  struct Cpu { ULONG id; int logical; int core; int eff; };
  std::vector<Cpu> cpus;
  int max_eff = 0;
  for (ULONG off = 0; off < len;) {
    auto* e = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
    if (e->Type == CpuSetInformation && e->CpuSet.Group == 0) {
      cpus.push_back({e->CpuSet.Id, e->CpuSet.LogicalProcessorIndex, e->CpuSet.CoreIndex,
                      e->CpuSet.EfficiencyClass});
      max_eff = std::max<int>(max_eff, e->CpuSet.EfficiencyClass);
    }
    off += e->Size;
  }
  int core0 = -1;
  for (const auto& c : cpus)
    if (c.logical == 0) core0 = c.core;
  for (const auto& c : cpus) {
    if (c.eff != max_eff || c.core == core0) continue;
    rc.ids.push_back(c.id);
    rc.logical.push_back(c.logical);
  }
  return rc;
}

const RtCores& Cores() {
  static std::once_flag once;
  static RtCores rc;
  std::call_once(once, [] { rc = Compute(); });
  return rc;
}

}

void DisableProcessPowerThrottling() {
  PROCESS_POWER_THROTTLING_STATE st{};
  st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                   PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  st.StateMask = 0;
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st));
}

void PinCurrentThreadToRealtimeCores() {
  THREAD_POWER_THROTTLING_STATE ts{};
  ts.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  ts.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  ts.StateMask = 0;
  SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &ts, sizeof(ts));
  const RtCores& rc = Cores();
  if (rc.ids.empty()) return;
  SetThreadSelectedCpuSets(GetCurrentThread(), rc.ids.data(), (ULONG)rc.ids.size());
}

std::string RealtimeCoresText() {
  const RtCores& rc = Cores();
  if (rc.logical.empty()) return "none";
  std::string s;
  for (int l : rc.logical) {
    if (!s.empty()) s += ",";
    s += std::to_string(l);
  }
  return s;
}

}
