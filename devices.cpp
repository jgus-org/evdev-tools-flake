#include "devices.hpp"

#include <dirent.h>
#include <linux/input-event-codes.h>
#include <sys/ioctl.h>

#include <cstring>

#include "util.hpp"

namespace evi {

Route route_key(std::uint16_t code) {
  if (code >= BTN_MOUSE && code <= BTN_TASK)
    return Route::pointer;
  if (code >= BTN_MISC && code < KEY_OK)
    return Route::drop;
  if (code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT)
    return Route::drop;
  if (code >= BTN_TRIGGER_HAPPY)
    return Route::drop;
  return Route::kbd;
}

std::vector<std::string> list_event_nodes() {
  std::vector<std::string> nodes;
  DIR* dir = opendir("/dev/input");
  if (!dir)
    return nodes;
  while (dirent* ent = readdir(dir))
    if (std::strncmp(ent->d_name, "event", 5) == 0 && ent->d_name[5] != '\0')
      nodes.push_back(std::string("/dev/input/") + ent->d_name);
  closedir(dir);
  return nodes;
}

bool device_has_keys(int fd, const std::vector<std::uint16_t>& codes) {
  unsigned char bits[KEY_MAX / 8 + 1] = {};
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0)
    return false;
  for (auto code : codes)
    if (!has_bit(bits, code))
      return false;
  return true;
}

std::vector<std::uint16_t> pressed_keys(int fd) {
  std::vector<std::uint16_t> keys;
  unsigned char bits[KEY_MAX / 8 + 1] = {};
  if (ioctl(fd, EVIOCGKEY(sizeof bits), bits) < 0)
    return keys;
  for (unsigned code = 0; code <= KEY_MAX; ++code)
    if (has_bit(bits, code))
      keys.push_back(static_cast<std::uint16_t>(code));
  return keys;
}

}  // namespace evi
