// Chord detection: buttons held together on one device fire a bound command once per hold.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace evi {

// Tracks a chord of codes held together on one device. is_due() holds from hold-time elapsed until mark_fired() or a release; the caller pairs the two to fire once per hold.
class ChordTracker {
 public:
  ChordTracker() = default;
  ChordTracker(std::vector<std::uint16_t> codes, std::chrono::milliseconds hold);
  void on_key(std::uint16_t code, std::int32_t value, std::chrono::steady_clock::time_point now);
  bool is_due(std::chrono::steady_clock::time_point now) const;
  void mark_fired();
  void reset();

 private:
  std::vector<std::uint16_t> codes_;
  std::chrono::milliseconds hold_{};
  std::vector<bool> down_;
  bool held_ = false;
  std::chrono::steady_clock::time_point held_since_{};
  bool fired_ = false;
};

// A chord bound to a command argv.
struct ChordSpec {
  std::vector<std::uint16_t> codes;
  std::chrono::milliseconds hold{0};
  std::vector<std::string> command;
};

// Parses a JSON chord list: [{"buttons": [evdev key codes], "holdMs": int, "command": [argv]}]; holdMs defaults to 0 (fire on press). Throws on malformed input.
std::vector<ChordSpec> parse_chord_specs(const std::string& json_text);

// One device's trackers over a chord list, indexed to match it.
class ChordSet {
 public:
  ChordSet() = default;
  explicit ChordSet(const std::vector<ChordSpec>& specs);
  void on_key(std::uint16_t code, std::int32_t value, std::chrono::steady_clock::time_point now);
  // Indices of chords whose hold has elapsed, each marked fired.
  std::vector<std::size_t> take_due(std::chrono::steady_clock::time_point now);

 private:
  std::vector<ChordTracker> trackers_;
};

}  // namespace evi
