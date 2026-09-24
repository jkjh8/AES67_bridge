#pragma once

#include <string>
#include <vector>

namespace aes67 {

struct AudioDevice {
  std::string id;
  std::string name;
  bool is_default = false;
  int channels = 0;
  int rate = 0;
};

std::vector<AudioDevice> EnumAudioDevices(bool render);

}
