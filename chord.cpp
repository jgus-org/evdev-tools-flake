#include "chord.hpp"

#include <algorithm>
#include <boost/json.hpp>

#include "util.hpp"

namespace evi {

std::vector<ChordSpec> parse_chord_specs(const std::string& json_text) {
  std::vector<ChordSpec> specs;
  const boost::json::value parsed = boost::json::parse(json_text);
  for (const auto& entry : parsed.as_array()) {
    const auto& obj = entry.as_object();
    ChordSpec spec;
    for (const auto& code : obj.at("buttons").as_array())
      spec.codes.push_back(static_cast<std::uint16_t>(code.as_int64()));
    if (const auto* hold = obj.if_contains("holdMs"))
      spec.hold = std::chrono::milliseconds(hold->as_int64());
    for (const auto& arg : obj.at("command").as_array()) spec.command.emplace_back(arg.as_string());
    specs.push_back(std::move(spec));
  }
  return specs;
}

ChordTracker::ChordTracker(std::vector<std::uint16_t> codes, std::chrono::milliseconds hold)
    : codes_(std::move(codes)),
      hold_(hold),
      down_(codes_.size(), false) {}

void ChordTracker::on_key(std::uint16_t code, std::int32_t value, std::chrono::steady_clock::time_point now) {
  if (value == kKeyAutorepeat)
    return;
  auto it = std::find(codes_.begin(), codes_.end(), code);
  if (it == codes_.end())
    return;
  down_[static_cast<std::size_t>(it - codes_.begin())] = value != kKeyRelease;
  if (std::all_of(down_.begin(), down_.end(), [](bool b) { return b; })) {
    if (!held_) {
      held_ = true;
      held_since_ = now;
    }
  } else {
    held_ = false;
    fired_ = false;
  }
}

bool ChordTracker::is_due(std::chrono::steady_clock::time_point now) const {
  return held_ && !fired_ && now - held_since_ >= hold_;
}

void ChordTracker::mark_fired() { fired_ = true; }

void ChordTracker::reset() {
  std::fill(down_.begin(), down_.end(), false);
  held_ = false;
  fired_ = false;
}

ChordSet::ChordSet(const std::vector<ChordSpec>& specs) {
  trackers_.reserve(specs.size());
  for (const auto& spec : specs) trackers_.emplace_back(spec.codes, spec.hold);
}

void ChordSet::on_key(std::uint16_t code, std::int32_t value, std::chrono::steady_clock::time_point now) {
  for (auto& tracker : trackers_) tracker.on_key(code, value, now);
}

std::vector<std::size_t> ChordSet::take_due(std::chrono::steady_clock::time_point now) {
  std::vector<std::size_t> due;
  for (std::size_t i = 0; i < trackers_.size(); ++i)
    if (trackers_[i].is_due(now)) {
      trackers_[i].mark_fired();
      due.push_back(i);
    }
  return due;
}

}  // namespace evi
