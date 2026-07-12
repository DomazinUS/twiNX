#pragma once

#include <cstdint>
#include <string>

namespace twinx::debug {

void init(const std::string& path);
void shutdown();
bool active();

void log(const char* category, const char* format, ...);

uint64_t hashText(const std::string& value);
const char* playerEventName(int event);

}  // namespace twinx::debug
