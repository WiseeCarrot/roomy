#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace roomy {

int runUi(HINSTANCE inst, int show, const std::vector<std::wstring>& args);

}  // namespace roomy
