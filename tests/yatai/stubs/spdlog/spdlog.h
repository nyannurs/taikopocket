// Stand-in for spdlog: YataiDON's parser and judge only log through it.
#pragma once
#include <string>
namespace spdlog {
template <typename... A> void debug(A&&...) {}
template <typename... A> void info(A&&...) {}
template <typename... A> void warn(A&&...) {}
template <typename... A> void error(A&&...) {}
} // namespace spdlog
