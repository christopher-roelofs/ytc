// Tiny cross-platform helpers so the rest of the code stays free of #ifdefs.
// Implemented per-OS in platform.cpp (Linux / Windows / macOS).
#pragma once
#include <string>

namespace platform {

// Absolute path of the running executable ("" if it can't be determined).
std::string exe_path();
// Directory containing the running executable (".", as a fallback).
std::string exe_dir();

} // namespace platform
