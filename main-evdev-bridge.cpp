// evdev-bridge: holds a bridge session open on the interposer's control socket for the life of the process; the interposer switches the TV to the target and forwards local input to it as CEC user-control commands until this process exits. SIGUSR1 (the Shield tile's pop-stack entry) relays a pop: it sends "return" to ask the interposer to switch the TV back, but the session still ends only when the interposer observes the TV back on k1 and reports "done".

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <boost/program_options.hpp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

volatile std::sig_atomic_t g_exit = 0;
void on_term(int) { g_exit = 1; }

volatile std::sig_atomic_t g_return = 0;
void on_return(int) { g_return = 1; }

bool valid_physical_address(const std::string& s) {
  unsigned a, b, c, d;
  char tail;
  return std::sscanf(s.c_str(), "%1x.%1x.%1x.%1x%c", &a, &b, &c, &d, &tail) == 4;
}

}  // namespace

int main(int argc, char** argv) {
  namespace po = boost::program_options;
  po::options_description desc("evdev-bridge options");
  // clang-format off
  desc.add_options()
      ("help,h", "show this help")
      ("socket", po::value<std::string>()->required(), "interposer control socket path")
      ("target", po::value<std::string>()->required(), "CEC physical address to bridge to, e.g. 3.0.0.0");
  // clang-format on

  std::string socket_path, target;
  try {
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    if (vm.count("help")) {
      std::cout << desc;
      return 0;
    }
    po::notify(vm);
    socket_path = vm["socket"].as<std::string>();
    target = vm["target"].as<std::string>();
  } catch (const po::error& e) {
    std::cerr << "evdev-bridge: " << e.what() << "\n" << desc;
    return 2;
  }
  if (!valid_physical_address(target)) {
    std::cerr << "evdev-bridge: target must be a CEC physical address (x.x.x.x)\n";
    return 2;
  }

  std::signal(SIGTERM, on_term);
  std::signal(SIGINT, on_term);
  std::signal(SIGHUP, on_term);
  std::signal(SIGUSR1, on_return);
  // Signals are held outside ppoll so a flag set between the loop's checks and the wait can't be missed.
  sigset_t held, waitmask;
  sigemptyset(&held);
  for (int s : {SIGTERM, SIGINT, SIGHUP, SIGUSR1}) sigaddset(&held, s);
  sigprocmask(SIG_BLOCK, &held, &waitmask);

  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un sa{};
  sa.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof sa.sun_path) {
    std::cerr << "evdev-bridge: socket path too long\n";
    return 2;
  }
  std::strcpy(sa.sun_path, socket_path.c_str());
  if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) < 0) {
    std::perror("evdev-bridge: connect");
    return 1;
  }
  std::string cmd = "bridge " + target + "\n";
  if (::write(fd, cmd.data(), cmd.size()) != static_cast<ssize_t>(cmd.size())) {
    std::perror("evdev-bridge: write");
    return 1;
  }

  while (!g_exit) {
    if (g_return) {
      g_return = 0;
      constexpr char kReturn[] = "return\n";
      if (::write(fd, kReturn, sizeof kReturn - 1) != static_cast<ssize_t>(sizeof kReturn - 1)) {
        std::perror("evdev-bridge: write return");
        return 1;
      }
    }
    pollfd pfd{fd, POLLIN, 0};
    int n = ::ppoll(&pfd, 1, nullptr, &waitmask);
    if (n < 0 && errno == EINTR)
      continue;
    if (pfd.revents & POLLIN) {
      char buf[64] = {};
      ssize_t r = ::read(fd, buf, sizeof buf - 1);
      if (r > 0 && std::strstr(buf, "done"))
        return 0;  // bridge completed: the TV is back on k1
      if (r <= 0)
        return 1;  // interposer went away
    }
    if (pfd.revents & (POLLHUP | POLLERR))
      return 1;
  }
  return 0;  // signal exit closes the socket, which ends the bridge
}
