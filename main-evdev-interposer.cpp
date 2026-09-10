#include <boost/program_options.hpp>
#include <chrono>
#include <cstdio>
#include <exception>
#include <iostream>
#include <istream>
#include <string>
#include <vector>

#include "daemon.hpp"

namespace evi {

// VID:PID[:NAME_SUBSTR], ids in hex. boost::po validates via lexical_cast, which drives this extractor.
std::istream& operator>>(std::istream& is, Matcher& m) {
  char sep = 0;
  is >> std::hex >> m.vendor >> sep >> m.product;
  if (!is || sep != ':') {
    is.setstate(std::ios::failbit);
    return is;
  }
  if (!is.eof()) {
    if (is.get() != ':') {
      is.setstate(std::ios::failbit);
      return is;
    }
    std::getline(is, m.name_substr.emplace());
  }
  return is;
}

}  // namespace evi

int main(int argc, char** argv) {
  setvbuf(stderr, nullptr, _IOLBF, 0);
  namespace po = boost::program_options;
  po::options_description desc("evdev-interposer options");
  // clang-format off
  desc.add_options()
      ("help,h", "show this help")
      ("interpose", po::value<std::vector<evi::Matcher>>()->composing(), "VID:PID[:NAME_SUBSTR] device to interpose (ids in hex); repeatable")
      ("remote", po::value<std::vector<std::string>>()->composing(), "name substring of a read-only pop/cursor source; repeatable")
      ("pop-key", po::value<std::vector<std::uint16_t>>()->composing(), "evdev key code that fires the pop command; repeatable")
      ("pop-chord", po::value<std::vector<std::uint16_t>>()->multitoken(), "pad chord: evdev key codes held together to fire the pop command")
      ("pop-hold-ms", po::value<int>()->default_value(0), "chord hold time before firing")
      ("pop-cmd", po::value<std::string>(), "command run on pop")
      ("pop-forward-key", po::value<std::uint16_t>(), "evdev key code emitted on the keyboard sink when the pop command exits 10")
      ("cec-adapter", po::value<std::string>(), "CEC adapter name for the TV gate")
      ("cec-osd-name", po::value<std::string>(), "claim the CEC playback logical address, presenting this OSD name")
      ("control-socket", po::value<std::string>(), "unix socket accepting bridge sessions (see evdev-bridge)")
      ("volume-socket", po::value<std::string>(), "unix datagram socket taking volume commands (up/down/mute) while no audio system is on the CEC bus");
  // clang-format on

  evi::Config cfg;
  try {
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    if (vm.count("help")) {
      std::cout << desc;
      return 0;
    }
    if (vm.count("interpose"))
      cfg.interpose = vm["interpose"].as<std::vector<evi::Matcher>>();
    if (vm.count("remote"))
      cfg.remotes = vm["remote"].as<std::vector<std::string>>();
    if (vm.count("pop-key"))
      cfg.pop_keys = vm["pop-key"].as<std::vector<std::uint16_t>>();
    if (vm.count("pop-chord"))
      cfg.pop_chord = vm["pop-chord"].as<std::vector<std::uint16_t>>();
    cfg.pop_hold = std::chrono::milliseconds(vm["pop-hold-ms"].as<int>());
    if (vm.count("pop-cmd"))
      cfg.pop_cmd = vm["pop-cmd"].as<std::string>();
    if (vm.count("pop-forward-key"))
      cfg.pop_forward_key = vm["pop-forward-key"].as<std::uint16_t>();
    if (vm.count("cec-adapter"))
      cfg.cec_adapter = vm["cec-adapter"].as<std::string>();
    if (vm.count("cec-osd-name"))
      cfg.cec_osd_name = vm["cec-osd-name"].as<std::string>();
    if (vm.count("control-socket"))
      cfg.control_socket = vm["control-socket"].as<std::string>();
    if (vm.count("volume-socket"))
      cfg.volume_socket = vm["volume-socket"].as<std::string>();
  } catch (const po::error& e) {
    std::cerr << "evdev-interposer: " << e.what() << "\n" << desc;
    return 2;
  }

  try {
    evi::run(cfg);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "evdev-interposer: %s\n", e.what());
    return 1;
  }
  return 0;
}
