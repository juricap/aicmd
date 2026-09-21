#pragma once
#include "core/core.h"
namespace ac {
struct StateLoad { SavedState state; bool found = false; Error error; };
StateLoad load_state(const fs::path&);
Error save_state(const fs::path&, const SavedState&);
fs::path default_state_path();
}
