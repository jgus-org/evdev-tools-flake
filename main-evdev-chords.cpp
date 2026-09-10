// evdev-chords CONFIG.json: passive chord observer. CONFIG is [{"buttons": [evdev key codes], "holdMs": int, "command": [argv]}]; holdMs defaults to 0 (fire on press). Fires a chord's command once per hold when all its buttons are held together on one device for holdMs. Devices advertising every button of at least one chord are discovered by rescanning /dev/input (no udev here) and read without grabbing.

#include <fcntl.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "chord.hpp"
#include "devices.hpp"
#include "unique_fd.hpp"
#include "util.hpp"

namespace {

using std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto kScanPeriod = 2s;
constexpr auto kTick = 100ms;

struct Device {
  evi::unique_fd fd;
  std::string path;
  evi::ChordSet chords;
};

std::vector<evi::ChordSpec> load_config(const char* path) {
  std::ifstream in(path);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return evi::parse_chord_specs(text);
}

void fire(const evi::ChordSpec& spec) {
  std::fprintf(stderr, "evdev-chords: %s\n", spec.command.front().c_str());
  if (::fork() == 0) {
    std::vector<char*> argv;
    argv.reserve(spec.command.size() + 1);
    for (const auto& arg : spec.command) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    _exit(127);
  }
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stderr, nullptr, _IOLBF, 0);
  if (argc != 2) {
    std::fprintf(stderr, "usage: evdev-chords CONFIG.json\n");
    return 2;
  }
  std::signal(SIGCHLD, SIG_IGN);  // auto-reap fired commands

  std::vector<evi::ChordSpec> specs;
  try {
    specs = load_config(argv[1]);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "evdev-chords: %s: %s\n", argv[1], e.what());
    return 2;
  }

  evi::unique_fd ep(epoll_create1(EPOLL_CLOEXEC));
  if (!ep) {
    std::fprintf(stderr, "evdev-chords: epoll_create1: %s\n", evi::errstr().c_str());
    return 1;
  }

  std::map<std::string, Device> devices;
  std::vector<std::string> dead;
  auto next_scan = steady_clock::time_point{};

  for (;;) {
    auto now = steady_clock::now();
    if (now >= next_scan) {
      next_scan = now + kScanPeriod;
      for (const auto& path : evi::list_event_nodes()) {
        if (devices.count(path))
          continue;
        evi::unique_fd fd(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        if (!fd)
          continue;
        bool wanted = false;
        for (const auto& spec : specs)
          if (evi::device_has_keys(fd.get(), spec.codes)) {
            wanted = true;
            break;
          }
        if (!wanted)
          continue;
        auto [it, inserted] = devices.try_emplace(path);
        Device& dev = it->second;
        dev.path = path;
        dev.chords = evi::ChordSet(specs);
        for (auto code : evi::pressed_keys(fd.get())) dev.chords.on_key(code, evi::kKeyPress, now);
        dev.fd = std::move(fd);
        epoll_event ee{};
        ee.events = EPOLLIN;
        ee.data.ptr = &dev;
        if (epoll_ctl(ep.get(), EPOLL_CTL_ADD, dev.fd.get(), &ee) < 0)
          devices.erase(it);
      }
    }

    std::array<epoll_event, 16> events;
    int n = epoll_wait(
        ep.get(),
        events.data(),
        static_cast<int>(events.size()),
        static_cast<int>(std::chrono::milliseconds(kTick).count()));
    for (int i = 0; i < n; ++i) {
      auto& dev = *static_cast<Device*>(events[i].data.ptr);
      std::array<input_event, 64> ev;
      for (;;) {
        ssize_t r = ::read(dev.fd.get(), ev.data(), sizeof ev);
        if (r < 0) {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          dead.push_back(dev.path);
          break;
        }
        if (r == 0)
          break;
        for (std::size_t j = 0, count = static_cast<std::size_t>(r) / sizeof(input_event); j < count; ++j)
          if (ev[j].type == EV_KEY)
            dev.chords.on_key(ev[j].code, ev[j].value, steady_clock::now());
      }
    }
    for (const auto& path : dead) {
      auto it = devices.find(path);
      if (it == devices.end())
        continue;
      epoll_ctl(ep.get(), EPOLL_CTL_DEL, it->second.fd.get(), nullptr);
      devices.erase(it);
    }
    dead.clear();

    now = steady_clock::now();
    for (auto& [path, dev] : devices)
      for (auto idx : dev.chords.take_due(now)) fire(specs[idx]);
  }
}
