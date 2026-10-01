#pragma once

namespace cw::trace {

// Installs breakpoints from CW_TRACE (comma-separated hex addresses) and starts the CW_WATCH memory logger.
void installFromEnvironment();
// Returns true when the exception was a tracepoint hit and execution can continue.
bool handle(EXCEPTION_POINTERS* info);

} // namespace cw::trace
