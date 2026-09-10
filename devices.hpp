// Shared evdev device plumbing: matching, discovery, capability queries, key routing, and deliberate-deflection detection.

#pragma once

#include <linux/input.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace evi {

// Selects real devices to interpose by evdev vendor/product id, optionally narrowed by a device-name substring (the 8BitDo dongle exposes pad, keyboard, and mouse nodes under one vid:pid).
struct Matcher {
  std::uint16_t vendor = 0;
  std::uint16_t product = 0;
  std::optional<std::string> name_substr;
};

enum class Route { kbd, pointer, drop };

// EV_KEY partition: BTN_MOUSE..BTN_TASK to the pointer sink, other BTN_* ranges dropped (pads are never interposed), everything else to the keyboard sink.
Route route_key(std::uint16_t code);

// Per-axis rest position sampled at device open, with a deflection threshold of a quarter of the axis range: sticks rest centered and triggers at their minimum, so a shared absolute threshold can't work. Digital axes (hats, range 2) get threshold 0 — any deflection counts.
struct AbsRest {
  std::int32_t rest = 0;
  std::int32_t threshold = 0;
};

inline bool is_deliberate(const AbsRest& axis, std::int32_t value) {
  return std::abs(value - axis.rest) > axis.threshold;
}

// /dev/input/eventN paths present right now.
std::vector<std::string> list_event_nodes();

// True when the device advertises every key in codes (EVIOCGBIT).
bool device_has_keys(int fd, const std::vector<std::uint16_t>& codes);

// Keys currently held on the device (EVIOCGKEY).
std::vector<std::uint16_t> pressed_keys(int fd);

}  // namespace evi
