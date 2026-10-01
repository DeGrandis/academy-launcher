#pragma once

namespace cw::options {

// Game-level options applied through hooks: view distance (CW_VIEW_DISTANCE, F7/F8).
void install();
// Steps the view distance multiplier down (-1) or up (+1); takes effect on the next frame.
void stepViewDistance(int direction);
// Re-applies changed options; called once per frame on the game thread.
void applyPending();
float viewDistance();

} // namespace cw::options
