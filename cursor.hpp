// Cursor-parking policy: non-pointer activity parks the cursor once (clearing hover) until real pointer motion returns; a session restart re-parks on the next key press.

#pragma once

#include <cstdint>

namespace evi {

// REL_X/REL_Y magnitude that pins the cursor into a corner. Hover follows the pointer position, so a highlighted element clears; the session compositor's idle timer hides the sprite.
inline constexpr std::int32_t kCursorWarp = 16384;

class CursorPark {
 public:
  // Non-pointer activity; true when the caller should emit the warp.
  bool note_key_activity() {
    if (!pointer_mode_ && !repark_)
      return false;
    pointer_mode_ = false;
    repark_ = false;
    return true;
  }

  void note_pointer_activity() { pointer_mode_ = true; }

  // A session (re)start resets the compositor's pointer state, leaving the cursor visible; park again on the next key press.
  void request_repark() { repark_ = true; }

 private:
  bool pointer_mode_ = true;
  bool repark_ = true;
};

}  // namespace evi
