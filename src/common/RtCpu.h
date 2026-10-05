#pragma once

#include <string>

namespace aes67 {

void DisableProcessPowerThrottling();
void PinCurrentThreadToRealtimeCores();
std::string RealtimeCoresText();

}
