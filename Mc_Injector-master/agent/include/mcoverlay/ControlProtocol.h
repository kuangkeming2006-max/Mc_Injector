#pragma once

#include <string_view>

namespace mcoverlay::protocol {

// The controller owns the named-pipe server and the agent connects as a
// client. The protocol is deliberately line-oriented so Qt can implement it
// without sharing an ABI-sensitive C structure. Every message ends in '\n'.
//
// Agent -> controller:
//   HELLO 1 <pid> <32-hex-token>\n
//   HOOK_READY OpenGL\n
//   RENDERER_READY OpenGL\n
//   STATE_APPLIED <visible:0|1> <interactive:0|1>\n
//   STATE_CHANGED <visible:0|1> <interactive:0|1>\n
//   DETACH_COMPLETE\n
//   STATUS <single-line-text>\n
//   ERROR <code> <single-line-text>\n
//
// Controller -> agent:
//   STATE <visible:0|1> <interactive:0|1>\n
//   DETACH\n
//
// A broken pipe hides the overlay and leaves the signed/normal JVM agent
// loaded. It never attempts FreeLibrary from inside the target process.
inline constexpr unsigned kProtocolVersion = 1U;
inline constexpr std::string_view kStateCommand = "STATE";
inline constexpr std::string_view kDetachCommand = "DETACH";
inline constexpr std::size_t kMaximumLineBytes = 1024;

} // namespace mcoverlay::protocol
