// The few symbols YataiDON's tja.cpp and judge.cpp need from the rest of the game.
#include <string>
#include <string_view>
#include <vector>

bool platform_read_file(const char*, std::vector<unsigned char>&) { return false; }
std::string md5_hexdigest(const std::vector<unsigned char>&) { return ""; }
