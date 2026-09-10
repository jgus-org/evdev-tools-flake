// The interposer daemon: presents virtual keyboard/pointer devices over grabbed evdev devices, gates all local input on the TV's CEC state, parks the cursor on non-pointer input, and fires a pop-one-level command from a key or pad chord. While gated, the pop gesture is the only input acted on: it wakes the TV and claims its input.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "devices.hpp"

namespace evi {

// Pop-command exit status requesting a pop_forward_key press.
inline constexpr int kPopForwardExit = 10;

struct Config {
  std::vector<Matcher> interpose;
  std::vector<std::string>
      remotes;  // name substrings of read-only pop/cursor sources (never grabbed, never wake the TV)
  std::vector<std::uint16_t> pop_keys;
  std::vector<std::uint16_t> pop_chord;
  std::chrono::milliseconds pop_hold{0};
  std::string pop_cmd;  // invoked with the pop trigger ("key" | "remote" | "pad chord") as its argument
  std::uint16_t pop_forward_key =
      0;                    // emitted press+release on the keyboard sink when pop_cmd exits kPopForwardExit; 0 disables
  std::string cec_adapter;  // empty disables the CEC gate/wake
  std::string cec_osd_name;    // OSD name for the daemon's logical-address claim; empty leaves claiming to others
  std::string control_socket;  // bridge-session control socket path; empty disables it
  std::string
      volume_socket;  // unix datagram sink taking volume commands ("up" | "down" | "mute") while no audio system is on the bus; empty keeps volume on CEC toward the TV
};

void run(const Config& cfg);

}  // namespace evi
