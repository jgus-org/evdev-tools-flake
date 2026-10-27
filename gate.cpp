#include "gate.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>

#include <bit>
#include <cstdio>
#include <cstring>

#include "util.hpp"

namespace evi {

namespace {

bool is_cec_node(std::string_view name) {
  return name.starts_with("cec") && name.size() > 3 &&
         name.find_first_not_of("0123456789", 3) == std::string_view::npos;
}

std::uint16_t physical_address(const cec_msg& message, std::size_t offset) {
  return static_cast<std::uint16_t>((message.msg[offset] << 8) | message.msg[offset + 1]);
}

}  // namespace

std::optional<std::string> select_cec_adapter(const std::vector<CecAdapterInfo>& adapters, std::string_view name) {
  for (const auto& adapter : adapters)
    if (adapter.name == name)
      return adapter.path;
  return std::nullopt;
}

std::optional<std::uint16_t> cec_routing_target(const cec_msg& message) {
  if (message.len < 2)
    return std::nullopt;
  switch (message.msg[1]) {
    case CEC_MSG_ACTIVE_SOURCE:
    case CEC_MSG_ROUTING_INFORMATION:
    case CEC_MSG_SET_STREAM_PATH:
      if (message.len >= 4)
        return physical_address(message, 2);
      break;
    case CEC_MSG_ROUTING_CHANGE:
      if (message.len >= 6)
        return physical_address(message, 4);
      break;
    default:
      break;
  }
  return std::nullopt;
}

void TvState::on_standby() { power_ = Power::standby; }

void TvState::on_power(std::uint8_t cec_status) {
  switch (cec_status) {
    case CEC_OP_POWER_STATUS_ON:
      power_ = Power::on;
      break;
    case CEC_OP_POWER_STATUS_STANDBY:
      power_ = Power::standby;
      break;
    case CEC_OP_POWER_STATUS_TO_ON:
      power_ = Power::to_on;
      break;
    case CEC_OP_POWER_STATUS_TO_STANDBY:
      power_ = Power::to_standby;
      break;
    default:
      break;
  }
}

void TvState::on_active(std::uint16_t physical_address) { active_physical_address_ = physical_address; }

void TvState::assume_live() {
  power_ = Power::on;
  active_physical_address_ = our_physical_address_;
}

bool TvState::is_gated() const {
  // Transitions gate too (no picture yet).
  if (power_ != Power::unknown && power_ != Power::on)
    return true;
  // Without our own physical address the active source can't be related to us: fail open.
  if (our_physical_address_ == kInvalidPhysicalAddress)
    return false;
  // Unknown active source with the TV known-on gates (fail safe: non-CEC inputs answer nothing, and a false gate costs one pop gesture — pop while gated wakes and re-announces); with power also unknown there is no evidence at all, so stay live.
  if (active_physical_address_ == kInvalidPhysicalAddress)
    return power_ == Power::on;
  return active_physical_address_ != our_physical_address_;
}

bool CecLink::open_adapter(const std::string& adapter_name, TvState& tv) {
  reset();
  DIR* directory = opendir("/dev");
  if (!directory)
    return false;
  std::vector<CecAdapterInfo> adapters;
  while (dirent* entry = readdir(directory)) {
    if (!is_cec_node(entry->d_name))
      continue;
    std::string candidate_path = std::string("/dev/") + entry->d_name;
    unique_fd candidate(::open(candidate_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
    if (!candidate)
      continue;
    cec_caps caps{};
    if (ioctl(candidate.get(), CEC_ADAP_G_CAPS, &caps) == 0)
      adapters.push_back({std::move(candidate_path), std::string(caps.name, strnlen(caps.name, sizeof caps.name))});
  }
  closedir(directory);
  auto path = select_cec_adapter(adapters, adapter_name);
  return path && open_path(*path, tv);
}

bool CecLink::open_path(const std::string& path, TvState& tv) {
  unique_fd mon(::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
  if (!mon)
    return false;
  std::uint32_t mode = CEC_MODE_NO_INITIATOR | CEC_MODE_MONITOR;
  if (ioctl(mon.get(), CEC_S_MODE, &mode) < 0) {
    std::fprintf(stderr, "evdev-interposer: CEC monitor mode failed: %s\n", errstr().c_str());
    return false;
  }
  unique_fd tx(::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
  if (!tx)
    return false;
  if (!claim_osd_.empty()) {
    std::uint32_t tx_mode = CEC_MODE_INITIATOR | CEC_MODE_FOLLOWER;
    if (ioctl(tx.get(), CEC_S_MODE, &tx_mode) < 0) {
      std::fprintf(stderr, "evdev-interposer: CEC follower mode failed: %s\n", errstr().c_str());
      return false;
    }
  }
  mon_ = std::move(mon);
  tx_ = std::move(tx);
  device_path_ = path;
  std::uint16_t address = CEC_PHYS_ADDR_INVALID;
  if (ioctl(mon_.get(), CEC_ADAP_G_PHYS_ADDR, &address) < 0) {
    std::fprintf(stderr, "evdev-interposer: CEC physical-address query failed: %s\n", errstr().c_str());
    reset();
    return false;
  }
  tv.set_our_physical_address(address);
  if (!claim_osd_.empty()) {
    cec_log_addrs la{};
    if (ioctl(tx_.get(), CEC_ADAP_G_LOG_ADDRS, &la) < 0) {
      std::fprintf(stderr, "evdev-interposer: CEC logical-address query failed: %s\n", errstr().c_str());
      reset();
      return false;
    }
    if (la.num_log_addrs > 0) {
      if (la.log_addr[0] != CEC_LOG_ADDR_INVALID)
        adopt(la.log_addr[0], tv);
    } else if (!submit_claim()) {
      reset();
      return false;
    }
  }
  return true;
}

void CecLink::reset() {
  mon_.reset();
  tx_.reset();
  device_path_.clear();
  failed_ = false;
  reset_protocol_state();
}

void CecLink::reset_protocol_state() {
  state_queried_ = false;
  la_ = -1;
  announced_ = false;
  bus_pa_.fill(kInvalidPhysicalAddress);
}

void CecLink::note_ioctl_failure() {
  if (errno == ENODEV || errno == ENXIO)
    failed_ = true;
}

void CecLink::adopt(int la, const TvState& tv) {
  la_ = la;
  std::fprintf(stderr, "evdev-interposer: CEC logical address %d\n", la);
  if (!announced_ && activate_on_claim_)
    announced_ = announce_active_source(tv);
  probe_audio();
}

void CecLink::on_state_change(std::uint16_t phys_addr, std::uint16_t la_mask, TvState& tv) {
  tv.set_our_physical_address(phys_addr);
  if (claim_osd_.empty())
    return;
  if (phys_addr == kInvalidPhysicalAddress) {
    tv.reset();
    reset_protocol_state();
    return;
  }
  if (la_mask == 0) {
    cec_log_addrs la{};
    if (ioctl(tx_.get(), CEC_ADAP_G_LOG_ADDRS, &la) < 0) {
      note_ioctl_failure();
      return;
    }
    if (la.num_log_addrs == 0 && !submit_claim())
      failed_ = true;
    return;
  }
  adopt(std::countr_zero(la_mask), tv);
}

bool CecLink::submit_claim() {
  cec_log_addrs la{};
  if (ioctl(tx_.get(), CEC_ADAP_S_LOG_ADDRS, &la) < 0) {
    std::fprintf(stderr, "evdev-interposer: CEC unclaim failed: %s\n", errstr().c_str());
    note_ioctl_failure();
    return false;
  }
  la = {};
  la.num_log_addrs = 1;
  la.cec_version = CEC_OP_CEC_VERSION_2_0;
  la.vendor_id = 0x000c03;  // HDMI LLC; the TV queries vendor id while (re)binding the input
  la.flags = CEC_LOG_ADDRS_FL_ALLOW_RC_PASSTHRU;
  std::snprintf(la.osd_name, sizeof la.osd_name, "%s", claim_osd_.c_str());
  la.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_PLAYBACK;
  la.log_addr_type[0] = CEC_LOG_ADDR_TYPE_PLAYBACK;
  la.all_device_types[0] = CEC_OP_ALL_DEVTYPE_PLAYBACK;
  if (ioctl(tx_.get(), CEC_ADAP_S_LOG_ADDRS, &la) < 0) {
    std::fprintf(stderr, "evdev-interposer: CEC claim failed: %s\n", errstr().c_str());
    note_ioctl_failure();
    return false;
  }
  return true;
}

void CecLink::drain_tx(std::uint32_t epoll_events) {
  if (epoll_events & (EPOLLERR | EPOLLHUP)) {
    failed_ = true;
    return;
  }
  cec_msg m{};
  while (ioctl(tx_.get(), CEC_RECEIVE, &m) == 0) {
    // sequence != 0 marks the status echo of our own transmit; broadcasts are never answered
    if (m.sequence == 0 && m.len >= 2 && (m.msg[0] & 0xf) != CEC_LOG_ADDR_BROADCAST) {
      std::uint8_t initiator = m.msg[0] >> 4;
      switch (m.msg[1]) {
        case CEC_MSG_GIVE_DEVICE_POWER_STATUS:
          send(CEC_MSG_REPORT_POWER_STATUS, initiator, {CEC_OP_POWER_STATUS_ON});
          break;
        case CEC_MSG_MENU_REQUEST:  // any request type: whatever is on screen is only ever the menu
          send(CEC_MSG_MENU_STATUS, initiator, {CEC_OP_MENU_STATE_ACTIVATED});
          break;
        case CEC_MSG_FEATURE_ABORT:         // never aborted (spec)
        case CEC_MSG_REPORT_POWER_STATUS:   // consumed on the monitor fd (gate)
        case CEC_MSG_STANDBY:               // consumed on the monitor fd (gate)
        case CEC_MSG_REPORT_PHYSICAL_ADDR:  // consumed on the monitor fd (bus map)
        case CEC_MSG_USER_CONTROL_PRESSED:  // consumed by kernel rc-cec
        case CEC_MSG_USER_CONTROL_RELEASED:
          break;
        default:
          send(CEC_MSG_FEATURE_ABORT, initiator, {m.msg[1], CEC_OP_ABORT_UNRECOGNIZED_OP});
          break;
      }
    }
    m = cec_msg{};
  }
  note_ioctl_failure();
}

bool CecLink::send(std::uint8_t opcode, std::uint8_t dest, std::initializer_list<std::uint8_t> params) {
  if (!can_send())
    return false;
  cec_log_addrs la{};
  if (ioctl(tx_.get(), CEC_ADAP_G_LOG_ADDRS, &la) < 0) {
    note_ioctl_failure();
    return false;
  }
  if (la.num_log_addrs == 0 || la.log_addr[0] == CEC_LOG_ADDR_INVALID)
    return false;
  cec_msg msg{};
  msg.msg[0] = static_cast<std::uint8_t>((la.log_addr[0] << 4) | dest);
  msg.msg[1] = opcode;
  msg.len = 2;
  for (std::uint8_t p : params) msg.msg[msg.len++] = p;
  if (ioctl(tx_.get(), CEC_TRANSMIT, &msg) < 0) {
    std::fprintf(stderr, "evdev-interposer: CEC transmit 0x%02x failed: %s\n", opcode, errstr().c_str());
    note_ioctl_failure();
    return false;
  }
  return true;
}

void CecLink::drain(std::uint32_t epoll_events, TvState& tv) {
  if (epoll_events & (EPOLLERR | EPOLLHUP)) {
    failed_ = true;
    return;
  }
  if (epoll_events & EPOLLPRI) {
    cec_event ev{};
    while (ioctl(mon_.get(), CEC_DQEVENT, &ev) == 0) {
      if (ev.event == CEC_EVENT_STATE_CHANGE)
        on_state_change(ev.state_change.phys_addr, ev.state_change.log_addr_mask, tv);
      ev = cec_event{};
    }
    note_ioctl_failure();
  }
  if (epoll_events & EPOLLIN) {
    cec_msg msg{};
    while (ioctl(mon_.get(), CEC_RECEIVE, &msg) == 0) {
      on_msg(msg, tv);
      msg = cec_msg{};
    }
    note_ioctl_failure();
  }
}

void CecLink::query_state() {
  if (state_queried_ || !tx_)
    return;
  if (!send(CEC_MSG_GIVE_DEVICE_POWER_STATUS, CEC_LOG_ADDR_TV, {}))
    return;
  if (!send(CEC_MSG_REQUEST_ACTIVE_SOURCE, CEC_LOG_ADDR_BROADCAST, {}))
    return;
  state_queried_ = true;
}

bool CecLink::announce_active_source(const TvState& tv) {
  std::uint16_t address = tv.our_physical_address();
  if (address == kInvalidPhysicalAddress)
    return false;
  return send(
      CEC_MSG_ACTIVE_SOURCE,
      CEC_LOG_ADDR_BROADCAST,
      {static_cast<std::uint8_t>(address >> 8), static_cast<std::uint8_t>(address & 0xff)});
}

void CecLink::note_bus_address(std::uint8_t la, std::uint16_t pa) {
  if (la < bus_pa_.size())
    bus_pa_[la] = pa;
}

int CecLink::la_for(std::uint16_t pa) const {
  if (pa == kInvalidPhysicalAddress)
    return -1;
  for (std::size_t la = 0; la < bus_pa_.size(); ++la)
    if (bus_pa_[la] == pa)
      return static_cast<int>(la);
  return -1;
}

void CecLink::probe_bus() {
  for (std::uint8_t la = 0; la < 15; ++la)
    if (!is_own_logical_address(la))
      send(CEC_MSG_GIVE_PHYSICAL_ADDR, la, {});
}

void CecLink::probe_audio() {
  if (bus_pa_[CEC_LOG_ADDR_AUDIOSYSTEM] == kInvalidPhysicalAddress)
    send(CEC_MSG_GIVE_PHYSICAL_ADDR, CEC_LOG_ADDR_AUDIOSYSTEM, {});
}

int CecLink::audio_la() const {
  return bus_pa_[CEC_LOG_ADDR_AUDIOSYSTEM] != kInvalidPhysicalAddress ? CEC_LOG_ADDR_AUDIOSYSTEM : CEC_LOG_ADDR_TV;
}

bool CecLink::switch_stream(std::uint16_t pa) {
  return send(
      CEC_MSG_ACTIVE_SOURCE,
      CEC_LOG_ADDR_BROADCAST,
      {static_cast<std::uint8_t>(pa >> 8), static_cast<std::uint8_t>(pa & 0xff)});
}

void CecLink::on_msg(const cec_msg& m, TvState& tv) {
  if (m.len < 2)
    return;
  std::uint8_t initiator = m.msg[0] >> 4;
  switch (m.msg[1]) {
    case CEC_MSG_STANDBY:
      if (initiator == CEC_LOG_ADDR_TV)
        tv.on_standby();
      break;
    case CEC_MSG_REPORT_POWER_STATUS:
      if (initiator == CEC_LOG_ADDR_TV && m.len >= 3)
        tv.on_power(m.msg[2]);
      break;
    case CEC_MSG_SET_STREAM_PATH:
      if (initiator == CEC_LOG_ADDR_TV) {
        if (auto target = cec_routing_target(m))
          tv.on_active(*target);
      }
      break;
    case CEC_MSG_ROUTING_CHANGE:
    case CEC_MSG_ROUTING_INFORMATION:
      if (auto target = cec_routing_target(m))
        tv.on_active(*target);
      break;
    case CEC_MSG_ACTIVE_SOURCE: {
      auto target = cec_routing_target(m);
      if (target) {
        tv.on_active(*target);
        if (!is_own_logical_address(initiator))
          note_bus_address(initiator, *target);
      }
      break;
    }
    case CEC_MSG_REPORT_PHYSICAL_ADDR:
      if (m.len >= 4 && !is_own_logical_address(initiator))
        note_bus_address(initiator, static_cast<std::uint16_t>((m.msg[2] << 8) | m.msg[3]));
      break;
    default:
      break;
  }
}

bool CecLink::is_own_logical_address(std::uint8_t address) {
  cec_log_addrs la{};
  if (ioctl(mon_.get(), CEC_ADAP_G_LOG_ADDRS, &la) < 0) {
    note_ioctl_failure();
    return false;
  }
  for (std::uint8_t i = 0; i < la.num_log_addrs; ++i)
    if (la.log_addr[i] == address)
      return true;
  return false;
}

}  // namespace evi
