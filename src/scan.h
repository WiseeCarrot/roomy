#pragma once

#include "model.h"

#include <functional>

namespace roomy {

Env detectEnv();
// Every location lives under one folder. Used by the tests and for demo screenshots.
Env sandboxEnv(const std::wstring& home);

// Runs every check in turn and hands each finished category to sink, empty ones included.
void scanAll(const Env& env, Progress& progress, const std::function<void(Category&&)>& sink);

}  // namespace roomy
