#include "daemon.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/cec.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <systemd/sd-daemon.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include "chord.hpp"
#include "cursor.hpp"
#include "gate.hpp"
#include "unique_fd.hpp"
#include "util.hpp"

namespace evi {
namespace {

using std::chrono::steady_clock;
using namespace std::chrono_literals;

volatile std::sig_atomic_t g_exit = 0;
void on_term(int) { g_exit = 1; }

struct Sink {
  unique_fd fd;
  std::set<std::uint16_t> pressed;
  bool pending = false;

  void emit(std::uint16_t type, std::uint16_t code, std::int32_t value) {
    input_event e{};
    e.type = type;
    e.code = code;
    e.value = value;
    if (::write(fd.get(), &e, sizeof e) != static_cast<ssize_t>(sizeof e))
      std::fprintf(stderr, "evdev-interposer: sink write failed: %s\n", errstr().c_str());
    if (type == EV_KEY) {
      switch (value) {
        case kKeyPress:
          pressed.insert(code);
          break;
        case kKeyRelease:
          pressed.erase(code);
          break;
        default:
          break;
      }
    }
    if (type != EV_SYN)
      pending = true;
  }

  void flush() {
    if (!pending)
      return;
    emit(EV_SYN, SYN_REPORT, 0);
    pending = false;
  }

  // Emits release for every held key so the gate can't strand a session with a stuck key.
  void release_all() {
    auto held = pressed;
    for (auto code : held) emit(EV_KEY, code, kKeyRelease);
    flush();
  }
};

unique_fd make_sink(bool keyboard, const char* name) {
  unique_fd fd(::open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC));
  if (!fd)
    throw std::runtime_error("open /dev/uinput: " + errstr());
  auto set = [&](unsigned long req, int val) {
    if (ioctl(fd.get(), req, val) < 0)
      throw std::runtime_error("uinput capability ioctl: " + errstr());
  };
  set(UI_SET_EVBIT, EV_KEY);
  if (keyboard) {
    for (int c = 1; c <= KEY_MAX; ++c)
      if (route_key(static_cast<std::uint16_t>(c)) == Route::kbd)
        set(UI_SET_KEYBIT, c);
    set(UI_SET_EVBIT, EV_MSC);
    set(UI_SET_MSCBIT, MSC_SCAN);
    set(UI_SET_EVBIT, EV_LED);
    for (int l : {LED_NUML, LED_CAPSL, LED_SCROLLL, LED_COMPOSE, LED_KANA}) set(UI_SET_LEDBIT, l);
    // Kernel-generated autorepeat; the real device's value-2 repeats are dropped in forwarding.
    set(UI_SET_EVBIT, EV_REP);
  } else {
    for (int c = BTN_MOUSE; c <= BTN_TASK; ++c) set(UI_SET_KEYBIT, c);
    set(UI_SET_EVBIT, EV_REL);
    for (int r : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES}) set(UI_SET_RELBIT, r);
  }
  uinput_setup us{};
  us.id.bustype = BUS_VIRTUAL;
  us.id.vendor = 0x0001;
  us.id.product = keyboard ? 0x0001 : 0x0002;
  us.id.version = 1;
  std::snprintf(us.name, sizeof us.name, "%s", name);
  if (ioctl(fd.get(), UI_DEV_SETUP, &us) < 0 || ioctl(fd.get(), UI_DEV_CREATE) < 0)
    throw std::runtime_error("uinput device create: " + errstr());
  return fd;
}

// The /dev/input/eventN node of a uinput device, via UI_GET_SYSNAME.
std::string sink_devnode(int uinput_fd) {
  char sysname[64] = {};
  if (ioctl(uinput_fd, UI_GET_SYSNAME(sizeof sysname), sysname) < 0)
    return {};
  std::string dir = std::string("/sys/devices/virtual/input/") + sysname;
  DIR* d = opendir(dir.c_str());
  if (!d)
    return {};
  std::string node;
  while (dirent* ent = readdir(d))
    if (std::strncmp(ent->d_name, "event", 5) == 0) {
      node = std::string("/dev/input/") + ent->d_name;
      break;
    }
  closedir(d);
  return node;
}

enum class Kind { interposed, pad, remote };

struct Source {
  unique_fd fd;
  std::string path;
  std::string name;
  Kind kind = Kind::interposed;
  bool grabbed = false;
  bool has_led = false;
  ChordTracker chord;
  std::map<std::uint16_t, AbsRest> abs_rest;  // pads only
  std::array<int, 2> hat_held = {-1, -1};     // UI command held per dpad hat axis while bridging
};

// CEC UI command forwarded for a local key while bridging; -1 = not forwarded. 0x46 (Pause) is the Android TV play/pause toggle.
int cec_ui_command(std::uint16_t code) {
  switch (code) {
    case KEY_ENTER:
    case KEY_KPENTER:
    case BTN_SOUTH:
      return 0x00;  // select
    case KEY_UP:
      return 0x01;
    case KEY_DOWN:
      return 0x02;
    case KEY_LEFT:
      return 0x03;
    case KEY_RIGHT:
      return 0x04;
    case KEY_HOME:
    case BTN_START:
      return 0x09;  // root menu
    case KEY_BACKSPACE:
    case KEY_ESC:
    case BTN_EAST:
      return 0x0d;  // exit
    case KEY_VOLUMEUP:
      return 0x41;
    case KEY_VOLUMEDOWN:
      return 0x42;
    case KEY_MUTE:
      return 0x43;
    case KEY_SPACE:
    case KEY_PLAYPAUSE:
      return 0x46;  // pause
    case KEY_REWIND:
      return 0x48;
    case KEY_FASTFORWARD:
      return 0x49;
    case KEY_NEXTSONG:
    case BTN_TR:
      return 0x4b;  // forward
    case KEY_PREVIOUSSONG:
    case BTN_TL:
      return 0x4c;  // backward
    default:
      return -1;
  }
}

// Volume UI commands address the audio device (System Audio Control), not the bridged target.
bool is_volume_ui_command(int cmd) { return cmd >= 0x41 && cmd <= 0x43; }

class Daemon {
 public:
  explicit Daemon(Config cfg) : cfg_(std::move(cfg)) {}

  void run() {
    std::signal(SIGTERM, on_term);
    std::signal(SIGINT, on_term);
    std::signal(SIGPIPE, SIG_IGN);
    // Pop commands are reaped through a signalfd in the epoll loop so their exit status is seen (kPopForwardExit requests a forward).
    sigset_t chld_mask;
    sigemptyset(&chld_mask);
    sigaddset(&chld_mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld_mask, nullptr);
    chld_.reset(signalfd(-1, &chld_mask, SFD_NONBLOCK | SFD_CLOEXEC));
    if (!chld_)
      throw std::runtime_error("signalfd: " + errstr());

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
      std::fprintf(stderr, "evdev-interposer: mlockall failed (%s); continuing\n", errstr().c_str());
    if (setpriority(PRIO_PROCESS, 0, -5) != 0)
      std::fprintf(stderr, "evdev-interposer: setpriority failed (%s); continuing\n", errstr().c_str());

    kbd_.fd = make_sink(true, "evdev-interposer keyboard");
    ptr_.fd = make_sink(false, "evdev-interposer pointer");

    ep_.reset(epoll_create1(EPOLL_CLOEXEC));
    if (!ep_)
      throw std::runtime_error("epoll_create1: " + errstr());
    ep_add(kbd_.fd.get(), &kbd_, EPOLLIN);
    ep_add(ptr_.fd.get(), &ptr_, EPOLLIN);
    ep_add(chld_.get(), &chld_, EPOLLIN);

    // A session (re)start resets the compositor's pointer state, leaving the cursor visible; its libinput opening our virtual nodes is the observable signal to re-park on the next key press.
    ino_.reset(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
    if (ino_) {
      for (const Sink* sink : {&kbd_, &ptr_}) {
        std::string node = sink_devnode(sink->fd.get());
        if (!node.empty() && inotify_add_watch(ino_.get(), node.c_str(), IN_OPEN) < 0)
          std::fprintf(stderr, "evdev-interposer: inotify watch %s failed: %s\n", node.c_str(), errstr().c_str());
      }
      ep_add(ino_.get(), &ino_, EPOLLIN);
    }

    if (!cfg_.control_socket.empty())
      ctl_setup();
    if (!cfg_.volume_socket.empty())
      vol_setup();
    cec_setup();
    scan();
    sd_notify(0, "READY=1");

    auto last_scan = steady_clock::now();
    std::array<epoll_event, 32> events;
    while (!g_exit) {
      int n = epoll_wait(ep_.get(), events.data(), static_cast<int>(events.size()), 500);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        throw std::runtime_error("epoll_wait: " + errstr());
      }
      for (int i = 0; i < n; ++i) {
        void* p = events[i].data.ptr;
        if (p == &kbd_ || p == &ptr_)
          handle_sink(*static_cast<Sink*>(p));
        else if (p == &ino_)
          handle_inotify();
        else if (p == &chld_)
          handle_pop_exits();
        else if (p == &cec_) {
          if (cec_)
            cec_.drain_tx(events[i].events);
        } else if (p == &ctl_listen_)
          handle_ctl_accept();
        else if (p == &ctl_conn_)
          handle_ctl_data();
        else if (p == nullptr)
          handle_cec(events[i].events);
        else
          handle_source(*static_cast<Source*>(p));
      }
      if (cec_.failed())
        cec_disconnected();
      for (const auto& path : dead_) drop_source(path);
      dead_.clear();

      auto now = steady_clock::now();
      for (auto& [path, src] : sources_)
        if (src.kind == Kind::pad && src.chord.is_due(now)) {
          src.chord.mark_fired();
          if (gated_)
            wake();
          else
            fire_pop("pad chord");
        }
      if (wake_pending_) {
        if (now >= wake_deadline_) {
          std::fprintf(stderr, "evdev-interposer: wake unconfirmed; failing open\n");
          tv_.assume_live();
          wake_pending_ = false;
          apply_gate();
        } else if (now >= next_power_poll_) {
          cec_.send(CEC_MSG_GIVE_DEVICE_POWER_STATUS, CEC_LOG_ADDR_TV, {});
          // The TV restores its previous input mid-boot, overriding the wake announcement; re-claim until live.
          cec_.announce_active_source(tv_);
          next_power_poll_ = now + kPowerPoll;
        }
      }
      if (bridging()) {
        if (bridge_return_requested_ && now >= next_bridge_return_attempt_) {
          cec_.announce_active_source(tv_);
          next_bridge_return_attempt_ = now + kBridgeRetry;
        } else if (!bridge_return_requested_ && !bridge_switched_ && now >= next_bridge_attempt_) {
          bridge_switched_ = cec_.switch_stream(bridge_pa_);
          next_bridge_attempt_ = now + kBridgeRetry;
        }
        if (bridge_reconnect_deadline_ && now >= *bridge_reconnect_deadline_)
          bridge_reconnect_timeout();
      }
      if (now - last_scan >= kScanPeriod) {
        scan();
        cec_.query_state();
        last_scan = now;
      }
      if (cec_.failed())
        cec_disconnected();
    }
  }

 private:
  static constexpr auto kScanPeriod = 2s;
  static constexpr auto kWakeTimeout = 10s;
  static constexpr auto kPowerPoll = 1s;
  static constexpr auto kPopDebounce = 1s;
  static constexpr auto kBridgeRetry = 1s;
  static constexpr auto kBridgeReconnectTimeout = 10s;

  bool bridging() const { return bridge_pa_ != kInvalidPhysicalAddress; }

  void ctl_setup() {
    ::unlink(cfg_.control_socket.c_str());
    ctl_listen_.reset(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!ctl_listen_)
      throw std::runtime_error("control socket: " + errstr());
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (cfg_.control_socket.size() >= sizeof sa.sun_path)
      throw std::runtime_error("control socket path too long: " + cfg_.control_socket);
    std::strcpy(sa.sun_path, cfg_.control_socket.c_str());
    if (::bind(ctl_listen_.get(), reinterpret_cast<sockaddr*>(&sa), sizeof sa) < 0 ||
        ::listen(ctl_listen_.get(), 1) < 0)
      throw std::runtime_error("control socket bind/listen: " + errstr());
    ::chmod(cfg_.control_socket.c_str(), 0666);  // clients run as the session user
    ep_add(ctl_listen_.get(), &ctl_listen_, EPOLLIN);
  }

  void handle_ctl_accept() {
    unique_fd fd(::accept4(ctl_listen_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
    if (!fd)
      return;
    if (ctl_conn_) {  // one bridge session at a time
      std::fprintf(stderr, "evdev-interposer: control connection rejected (busy)\n");
      return;
    }
    ctl_conn_ = std::move(fd);
    ep_add(ctl_conn_.get(), &ctl_conn_, EPOLLIN);
  }

  // The connection is the bridge session: its close, however it happens, ends the bridge. A "return" line — the client relaying a pop (the Shield tile's pop-stack entry sends it SIGUSR1) — requests the TV back; the bridge still ends only when the TV is observed back on k1 (see handle_cec), which also covers it being switched back by any other means. Each request is one announcement; no blind retry.
  void handle_ctl_data() {
    char buf[64];
    ssize_t n = ::read(ctl_conn_.get(), buf, sizeof buf - 1);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
      return;
    if (n <= 0) {
      ctl_conn_.reset();
      bridge_off();
      return;
    }
    buf[n] = 0;
    unsigned a, b, c, d;
    if (std::sscanf(buf, "bridge %1x.%1x.%1x.%1x", &a, &b, &c, &d) == 4) {
      bridge_on(static_cast<std::uint16_t>(a << 12 | b << 8 | c << 4 | d));
    } else if (std::strncmp(buf, "return", 6) == 0 && bridging()) {
      std::fprintf(stderr, "evdev-interposer: bridge client requests return to k1\n");
      bridge_return_requested_ = true;
      next_bridge_return_attempt_ = steady_clock::now();
      cec_.set_activate_on_claim(true);
      cec_.announce_active_source(tv_);
    } else {
      std::fprintf(stderr, "evdev-interposer: control command not understood\n");
    }
  }

  void bridge_on(std::uint16_t pa) {
    std::fprintf(
        stderr,
        "evdev-interposer: bridging input to %x.%x.%x.%x\n",
        pa >> 12,
        (pa >> 8) & 0xf,
        (pa >> 4) & 0xf,
        pa & 0xf);
    bridge_pa_ = pa;
    bridge_switched_ = false;
    bridge_seen_away_ = false;
    bridge_return_requested_ = false;
    cec_.set_activate_on_claim(false);
    auto now = steady_clock::now();
    next_bridge_attempt_ = now;
    next_bridge_probe_ = now + kBridgeRetry;
    next_bridge_return_attempt_ = now;
    if (!cec_)
      bridge_reconnect_deadline_ = now + kBridgeReconnectTimeout;
    if (cec_.la_for(pa) < 0)
      cec_.probe_bus();  // covers the audio system too
    else
      cec_.probe_audio();
    apply_gate();
  }

  // State cleanup shared by every bridge ending. An abnormal end (client death without a completed return) deliberately leaves the TV alone: the gate engages and the next pop wakes and reclaims.
  void bridge_off() {
    if (!bridging())
      return;
    std::fprintf(stderr, "evdev-interposer: bridge closed\n");
    int la = cec_.la_for(bridge_pa_);
    if (la >= 0)
      cec_.send(CEC_MSG_USER_CONTROL_RELEASED, static_cast<std::uint8_t>(la), {});  // release anything left held
    bridge_pa_ = kInvalidPhysicalAddress;
    bridge_seen_away_ = false;
    bridge_return_requested_ = false;
    bridge_reconnect_deadline_.reset();
    cec_.set_activate_on_claim(true);
    for (auto& [path, src] : sources_) src.hat_held = {-1, -1};
    apply_gate();
  }

  // Target's logical address; while unknown, probes the bus (rate-limited).
  int bridge_la() {
    int la = cec_.la_for(bridge_pa_);
    if (la < 0) {
      auto now = steady_clock::now();
      if (now >= next_bridge_probe_) {
        cec_.probe_bus();
        next_bridge_probe_ = now + kBridgeRetry;
      }
    }
    return la;
  }

  void bridge_key(std::uint16_t code, std::int32_t value) {
    int cmd = cec_ui_command(code);
    if (cmd < 0)
      return;
    if (is_volume_ui_command(cmd)) {
      send_volume(cmd, value);
      return;
    }
    int la = bridge_la();
    if (la < 0)
      return;
    if (value == kKeyRelease)
      cec_.send(CEC_MSG_USER_CONTROL_RELEASED, static_cast<std::uint8_t>(la), {});
    else  // press and autorepeat: CEC repeat is a re-sent press
      cec_.send(CEC_MSG_USER_CONTROL_PRESSED, static_cast<std::uint8_t>(la), {static_cast<std::uint8_t>(cmd)});
  }

  // An audio system on the bus gets System Audio Control UCP; otherwise the volume sink, when configured, reaches the TV out of band (the LG ACKs and discards UCP volume addressed to it), with CEC toward the TV as the unconfigured fallback.
  void send_volume(int cmd, std::int32_t value) {
    int la = cec_.can_send() ? cec_.audio_la() : CEC_LOG_ADDR_TV;
    if (la == CEC_LOG_ADDR_TV && vol_fd_) {
      if (value == kKeyRelease)
        return;  // the sink takes one command per press/repeat; releases carry nothing
      const char* what = cmd == 0x41 ? "up" : cmd == 0x42 ? "down" : "mute";
      if (::sendto(
              vol_fd_.get(),
              what,
              std::strlen(what),
              0,
              reinterpret_cast<const sockaddr*>(&vol_addr_),
              sizeof vol_addr_) < 0) {
        if (errno != vol_errno_) {  // log error-state changes, not every autorepeat
          vol_errno_ = errno;
          std::fprintf(stderr, "evdev-interposer: volume sink send failed: %s\n", errstr().c_str());
        }
      } else {
        vol_errno_ = 0;
      }
      return;
    }
    if (!cec_.can_send())
      return;
    if (value == kKeyRelease)
      cec_.send(CEC_MSG_USER_CONTROL_RELEASED, static_cast<std::uint8_t>(la), {});
    else
      cec_.send(CEC_MSG_USER_CONTROL_PRESSED, static_cast<std::uint8_t>(la), {static_cast<std::uint8_t>(cmd)});
  }

  void vol_setup() {
    if (cfg_.volume_socket.size() >= sizeof vol_addr_.sun_path)
      throw std::runtime_error("volume socket path too long: " + cfg_.volume_socket);
    vol_fd_.reset(::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!vol_fd_)
      throw std::runtime_error("volume socket: " + errstr());
    vol_addr_.sun_family = AF_UNIX;
    // sendto re-resolves the path per datagram, so a sink restart (fresh socket inode at the same path) needs no handling here.
    std::strcpy(vol_addr_.sun_path, cfg_.volume_socket.c_str());
  }

  void bridge_hat(Source& src, const input_event& e) {
    int axis = e.code == ABS_HAT0Y;
    int cmd = e.value < 0 ? (axis ? 0x01 : 0x03) : e.value > 0 ? (axis ? 0x02 : 0x04) : -1;
    int& held = src.hat_held[axis];
    if (cmd == held)
      return;
    int la = bridge_la();
    if (la < 0) {
      held = -1;
      return;
    }
    if (held >= 0 && cmd < 0)
      cec_.send(CEC_MSG_USER_CONTROL_RELEASED, static_cast<std::uint8_t>(la), {});
    if (cmd >= 0)
      cec_.send(CEC_MSG_USER_CONTROL_PRESSED, static_cast<std::uint8_t>(la), {static_cast<std::uint8_t>(cmd)});
    held = cmd;
  }

  void ep_add(int fd, void* tag, std::uint32_t evmask) {
    epoll_event ee{};
    ee.events = evmask;
    ee.data.ptr = tag;
    if (epoll_ctl(ep_.get(), EPOLL_CTL_ADD, fd, &ee) < 0)
      throw std::runtime_error("epoll_ctl add: " + errstr());
  }

  bool match_interpose(const input_id& id, const char* name) const {
    for (const auto& m : cfg_.interpose)
      if (id.vendor == m.vendor && id.product == m.product &&
          (!m.name_substr || std::strstr(name, m.name_substr->c_str())))
        return true;
    return false;
  }

  bool match_remote(const char* name) const {
    for (const auto& r : cfg_.remotes)
      if (std::strstr(name, r.c_str()))
        return true;
    return false;
  }

  bool is_pop_key(std::uint16_t code) const {
    return std::find(cfg_.pop_keys.begin(), cfg_.pop_keys.end(), code) != cfg_.pop_keys.end();
  }

  void scan() {
    for (const auto& path : list_event_nodes())
      if (!sources_.count(path))
        probe(path);
    if (!cec_)
      cec_setup();
  }

  void probe(const std::string& path) {
    unique_fd fd(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (!fd)
      return;
    input_id id{};
    char name[256] = {};
    if (ioctl(fd.get(), EVIOCGID, &id) < 0 || ioctl(fd.get(), EVIOCGNAME(sizeof name - 1), name) < 0)
      return;
    unsigned char keys[KEY_MAX / 8 + 1] = {};
    ioctl(fd.get(), EVIOCGBIT(EV_KEY, sizeof keys), keys);

    // A gamepad is never interposed, even when a matcher covers its vid:pid.
    Kind kind;
    if (has_bit(keys, BTN_SOUTH))
      kind = Kind::pad;
    else if (match_interpose(id, name))
      kind = Kind::interposed;
    else if (match_remote(name))
      kind = Kind::remote;
    else
      return;

    bool grabbed = false;
    if (kind == Kind::interposed) {
      if (ioctl(fd.get(), EVIOCGRAB, 1) < 0) {
        if (grab_blocked_.insert(path).second)
          std::fprintf(
              stderr,
              "evdev-interposer: grab %s (%s) blocked: %s; retrying\n",
              path.c_str(),
              name,
              errstr().c_str());
        return;
      }
      grab_blocked_.erase(path);
      grabbed = true;
    } else if (kind == Kind::pad && gated_) {
      grabbed = ioctl(fd.get(), EVIOCGRAB, 1) == 0;
    }

    auto [it, inserted] = sources_.try_emplace(path);
    Source& src = it->second;
    src.path = path;
    src.name = name;
    src.kind = kind;
    src.grabbed = grabbed;
    if (kind == Kind::interposed) {
      unsigned char leds[LED_MAX / 8 + 1] = {};
      src.has_led = ioctl(fd.get(), EVIOCGBIT(EV_LED, sizeof leds), leds) >= 0 &&
                    std::any_of(std::begin(leds), std::end(leds), [](unsigned char b) { return b != 0; });
    }
    if (kind == Kind::pad && !cfg_.pop_chord.empty())
      src.chord = ChordTracker(cfg_.pop_chord, cfg_.pop_hold);
    if (kind == Kind::pad) {
      unsigned char abs_bits[ABS_MAX / 8 + 1] = {};
      if (ioctl(fd.get(), EVIOCGBIT(EV_ABS, sizeof abs_bits), abs_bits) >= 0)
        for (unsigned code = 0; code <= ABS_MAX; ++code) {
          if (!has_bit(abs_bits, code))
            continue;
          input_absinfo info{};
          if (ioctl(fd.get(), EVIOCGABS(code), &info) == 0)
            src.abs_rest[static_cast<std::uint16_t>(code)] = {info.value, (info.maximum - info.minimum) / 4};
        }
    }
    src.fd = std::move(fd);

    epoll_event ee{};
    ee.events = EPOLLIN;
    ee.data.ptr = &src;
    if (epoll_ctl(ep_.get(), EPOLL_CTL_ADD, src.fd.get(), &ee) < 0) {
      sources_.erase(it);
      return;
    }
    const char* what = kind == Kind::interposed ? "interposing"
                       : kind == Kind::pad      ? "watching pad"
                                                : "watching remote";
    std::fprintf(stderr, "evdev-interposer: %s %s (%s)\n", what, path.c_str(), name);
  }

  void drop_source(const std::string& path) {
    auto it = sources_.find(path);
    if (it == sources_.end())
      return;
    epoll_ctl(ep_.get(), EPOLL_CTL_DEL, it->second.fd.get(), nullptr);
    std::fprintf(stderr, "evdev-interposer: dropped %s (%s)\n", path.c_str(), it->second.name.c_str());
    sources_.erase(it);
  }

  void handle_source(Source& src) {
    std::array<input_event, 64> ev;
    bool have_scan = false;
    std::int32_t scan = 0;
    for (;;) {
      ssize_t n = ::read(src.fd.get(), ev.data(), sizeof ev);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        dead_.push_back(src.path);
        return;
      }
      if (n == 0)
        break;
      for (std::size_t i = 0, count = static_cast<std::size_t>(n) / sizeof(input_event); i < count; ++i) {
        const auto& e = ev[i];
        switch (src.kind) {
          case Kind::interposed:
            on_interposed(e, have_scan, scan);
            break;
          case Kind::pad:
            on_pad(src, e);
            break;
          case Kind::remote:
            on_remote(e);
            break;
        }
      }
    }
    kbd_.flush();
    ptr_.flush();
  }

  void on_interposed(const input_event& e, bool& have_scan, std::int32_t& scan) {
    if (e.type == EV_SYN) {
      if (e.code == SYN_REPORT) {
        kbd_.flush();
        ptr_.flush();
      }
      have_scan = false;
      return;
    }
    // Volume never enters the session: the audio device owns it in every state (gated, bridging, live).
    if (e.type == EV_KEY && (vol_fd_ || cec_.can_send())) {
      int cmd = cec_ui_command(e.code);
      if (is_volume_ui_command(cmd)) {
        send_volume(cmd, e.value);
        return;
      }
    }
    if (gated_) {
      if (e.type == EV_KEY && e.value == kKeyPress && is_pop_key(e.code))
        wake();
      return;
    }
    if (e.type == EV_MSC && e.code == MSC_SCAN) {
      have_scan = true;
      scan = e.value;
      return;
    }
    if (e.type == EV_KEY) {
      Route r = route_key(e.code);
      if (r == Route::drop)
        return;
      if (r == Route::kbd && is_pop_key(e.code)) {  // swallowed: a pop key must not leak into the session
        if (e.value == kKeyPress)
          fire_pop("key");
        return;
      }
      if (bridging()) {
        bridge_key(e.code, e.value);
        return;
      }
      if (e.value == kKeyAutorepeat)
        return;
      Sink& sink = r == Route::kbd ? kbd_ : ptr_;
      if (have_scan) {
        sink.emit(EV_MSC, MSC_SCAN, scan);
        have_scan = false;
      }
      sink.emit(EV_KEY, e.code, e.value);
      if (e.value == kKeyPress) {
        if (r == Route::kbd)
          note_key_activity();
        else
          note_pointer_activity();
      }
      return;
    }
    if (e.type == EV_REL) {
      if (bridging())
        return;
      note_pointer_activity();
      ptr_.emit(EV_REL, e.code, e.value);
    }
  }

  void on_pad(Source& src, const input_event& e) {
    if (e.type == EV_ABS) {
      // Hats bypass the deliberate-motion filter: the return to rest is the release edge.
      if (bridging() && (e.code == ABS_HAT0X || e.code == ABS_HAT0Y)) {
        bridge_hat(src, e);
        return;
      }
      auto it = src.abs_rest.find(e.code);
      if (it == src.abs_rest.end() || !is_deliberate(it->second, e.value))
        return;
      if (!gated_)
        note_key_activity();
      return;
    }
    if (e.type != EV_KEY)
      return;
    if (!gated_ && e.value == kKeyPress)
      note_key_activity();
    src.chord.on_key(e.code, e.value, steady_clock::now());
    if (bridging())
      bridge_key(e.code, e.value);
  }

  void on_remote(const input_event& e) {
    if (e.type != EV_KEY || e.value != kKeyPress || gated_)
      return;
    if (is_pop_key(e.code))
      fire_pop("remote");
    else
      note_key_activity();
  }

  // Consumers write LED state to the virtual keyboard; mirror it onto the real devices.
  void handle_sink(Sink& sink) {
    std::array<input_event, 16> ev;
    for (;;) {
      ssize_t n = ::read(sink.fd.get(), ev.data(), sizeof ev);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (n == 0)
        break;
      for (std::size_t i = 0, count = static_cast<std::size_t>(n) / sizeof(input_event); i < count; ++i) {
        const auto& e = ev[i];
        if (e.type != EV_LED)
          continue;
        for (auto& [path, src] : sources_) {
          if (src.kind != Kind::interposed || !src.has_led)
            continue;
          input_event out{};
          out.type = EV_LED;
          out.code = e.code;
          out.value = e.value;
          ::write(src.fd.get(), &out, sizeof out);
          out = input_event{};
          out.type = EV_SYN;
          out.code = SYN_REPORT;
          ::write(src.fd.get(), &out, sizeof out);
        }
      }
    }
  }

  void note_key_activity() {
    if (!park_.note_key_activity())
      return;
    ptr_.emit(EV_REL, REL_X, kCursorWarp);
    ptr_.emit(EV_REL, REL_Y, kCursorWarp);
    ptr_.flush();
  }

  void note_pointer_activity() { park_.note_pointer_activity(); }

  void handle_inotify() {
    char buf[512];
    while (::read(ino_.get(), buf, sizeof buf) > 0) {
    }
    park_.request_repark();
  }

  void fire_pop(const char* trigger) {
    if (cfg_.pop_cmd.empty())
      return;
    auto now = steady_clock::now();
    if (now - last_pop_ < kPopDebounce)
      return;
    last_pop_ = now;
    std::fprintf(stderr, "evdev-interposer: pop (%s)\n", trigger);
    pid_t pid = ::fork();
    if (pid == 0) {
      sigset_t empty;
      sigemptyset(&empty);
      sigprocmask(SIG_SETMASK, &empty, nullptr);  // the daemon's blocked SIGCHLD must not leak into the command
      ::execl(cfg_.pop_cmd.c_str(), cfg_.pop_cmd.c_str(), trigger, static_cast<char*>(nullptr));
      _exit(127);
    }
    if (pid < 0)
      std::fprintf(stderr, "evdev-interposer: fork for pop failed: %s\n", errstr().c_str());
  }

  void handle_pop_exits() {
    signalfd_siginfo si;
    while (::read(chld_.get(), &si, sizeof si) == sizeof si) {
    }
    int status = 0;
    while (::waitpid(-1, &status, WNOHANG) > 0)
      if (WIFEXITED(status) && WEXITSTATUS(status) == kPopForwardExit)
        forward_pop_key();
  }

  // A Moonlight stream owns the pop (the k1-side command exits kPopForwardExit): press the canonical pop key on the virtual keyboard, which Moonlight forwards to the remote session's listener.
  void forward_pop_key() {
    if (!cfg_.pop_forward_key)
      return;
    std::fprintf(stderr, "evdev-interposer: forwarding pop key into the stream\n");
    kbd_.emit(EV_KEY, cfg_.pop_forward_key, kKeyPress);
    kbd_.flush();
    kbd_.emit(EV_KEY, cfg_.pop_forward_key, kKeyRelease);
    kbd_.flush();
  }

  void wake() {
    if (wake_pending_ || !cec_.can_send())
      return;
    std::fprintf(stderr, "evdev-interposer: pop while gated; waking TV\n");
    cec_.send(CEC_MSG_IMAGE_VIEW_ON, CEC_LOG_ADDR_TV, {});
    cec_.announce_active_source(tv_);
    wake_pending_ = true;
    auto now = steady_clock::now();
    wake_deadline_ = now + kWakeTimeout;
    next_power_poll_ = now + kPowerPoll;
  }

  void cec_setup() {
    if (cfg_.cec_adapter.empty() || cec_)
      return;
    cec_.claim_as(cfg_.cec_osd_name);
    cec_.set_activate_on_claim(!bridging() || bridge_return_requested_);
    if (!cec_.open_adapter(cfg_.cec_adapter, tv_))
      return;
    ep_add(cec_.monitor_fd(), nullptr, EPOLLIN | EPOLLPRI);
    if (!cfg_.cec_osd_name.empty())
      ep_add(cec_.tx_fd(), &cec_, EPOLLIN);
    cec_.query_state();
    if (bridge_return_requested_)
      cec_.announce_active_source(tv_);
    std::uint16_t address = tv_.our_physical_address();
    std::fprintf(
        stderr,
        "evdev-interposer: CEC adapter %s on %s (physical address %x.%x.%x.%x)\n",
        cfg_.cec_adapter.c_str(),
        cec_.device_path().c_str(),
        address >> 12,
        (address >> 8) & 0xf,
        (address >> 4) & 0xf,
        address & 0xf);
  }

  void cec_disconnected() {
    std::string path = cec_.device_path();
    cec_.reset();
    tv_.reset();
    wake_pending_ = false;
    if (bridging() && !bridge_reconnect_deadline_)
      bridge_reconnect_deadline_ = steady_clock::now() + kBridgeReconnectTimeout;
    std::fprintf(stderr, "evdev-interposer: CEC adapter disconnected (%s)\n", path.c_str());
    apply_gate();
  }

  void handle_cec(std::uint32_t events) {
    if (!cec_)
      return;
    cec_.drain(events, tv_);
    if (bridging() && tv_.has_active_source()) {
      if (!tv_.is_active_source()) {
        bridge_seen_away_ = true;
        bridge_reconnect_deadline_.reset();
      } else if (bridge_seen_away_ || bridge_return_requested_) {
        bridge_complete();
      }
    }
    apply_gate();
  }

  void bridge_complete() { finish_bridge("TV back on k1; ending bridge"); }

  void bridge_reconnect_timeout() { finish_bridge("CEC unavailable; ending bridge"); }

  void finish_bridge(const char* message) {
    std::fprintf(stderr, "evdev-interposer: %s\n", message);
    if (ctl_conn_) {
      constexpr char kDone[] = "done\n";
      if (::write(ctl_conn_.get(), kDone, sizeof kDone - 1) < 0 && errno != EPIPE)
        std::fprintf(stderr, "evdev-interposer: bridge done notify failed: %s\n", errstr().c_str());
      ctl_conn_.reset();
    }
    bridge_off();
  }

  void apply_gate() {
    // A bridge session forces live: the TV is deliberately on another input while local devices drive it.
    bool want = cec_ && tv_.is_gated() && !bridging();
    if (want == gated_)
      return;
    gated_ = want;
    std::fprintf(stderr, "evdev-interposer: %s\n", gated_ ? "gated (TV off or on another input)" : "live");
    if (gated_) {
      kbd_.release_all();
      ptr_.release_all();
      for (auto& [path, src] : sources_) {
        if (src.kind != Kind::pad)
          continue;
        src.chord.reset();
        if (!src.grabbed)
          src.grabbed = ioctl(src.fd.get(), EVIOCGRAB, 1) == 0;
      }
    } else {
      wake_pending_ = false;
      for (auto& [path, src] : sources_) {
        if (src.kind != Kind::pad)
          continue;
        // Chords also track while gated (they wake the TV); in-flight state must not fire a pop into the session the gate just revealed.
        src.chord.reset();
        if (src.grabbed) {
          ioctl(src.fd.get(), EVIOCGRAB, 0);
          src.grabbed = false;
        }
      }
    }
  }

  Config cfg_;
  unique_fd ep_;
  Sink kbd_, ptr_;
  std::map<std::string, Source> sources_;
  std::set<std::string> grab_blocked_;
  std::vector<std::string> dead_;
  CecLink cec_;
  TvState tv_;
  bool gated_ = false;
  bool wake_pending_ = false;
  steady_clock::time_point wake_deadline_;
  steady_clock::time_point next_power_poll_;
  CursorPark park_;
  unique_fd ino_;
  unique_fd chld_;
  steady_clock::time_point last_pop_;
  unique_fd ctl_listen_, ctl_conn_;
  unique_fd vol_fd_;
  sockaddr_un vol_addr_{};
  int vol_errno_ = 0;
  std::uint16_t bridge_pa_ = kInvalidPhysicalAddress;
  bool bridge_switched_ = false;
  bool bridge_seen_away_ = false;
  bool bridge_return_requested_ = false;
  steady_clock::time_point next_bridge_attempt_;
  steady_clock::time_point next_bridge_probe_;
  steady_clock::time_point next_bridge_return_attempt_;
  std::optional<steady_clock::time_point> bridge_reconnect_deadline_;
};

}  // namespace

void run(const Config& cfg) {
  Daemon daemon(cfg);
  daemon.run();
}

}  // namespace evi
