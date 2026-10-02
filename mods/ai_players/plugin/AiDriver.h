#pragma once

#include "cw_mod.h"

// In a match: what a computer player's gamepad does.
namespace ai {

void install(const CwModApi* api);
// A new mission is loading.
void missionStart(const char* mission);
// Fills in computer player `slot`'s gamepad for this poll.
void drive(int slot, CwPad* pad);

} // namespace ai
