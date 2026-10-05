#pragma once

#include <filesystem>
#include <string>

#include "common/Types.h"

namespace aes67 {

std::filesystem::path DefaultConfigPath();

AppConfig LoadConfig(const std::filesystem::path& path, bool* used_defaults = nullptr);

bool SaveConfig(const std::filesystem::path& path, const AppConfig& cfg);

std::string ValidateConfig(const AppConfig& cfg);
std::string ValidateTx(const TxConfig& t);
std::string ValidateRx(const RxConfig& r);

}
