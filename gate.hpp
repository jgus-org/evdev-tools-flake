// TV-side input gate: power/active-source state derived from CEC bus traffic, plus the CEC I/O that feeds and acts on it.

#pragma once

#include <linux/cec.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "unique_fd.hpp"

namespace evi {

inline constexpr std::uint16_t kInvalidPhysicalAddress = 0xffff;

// TV power + active-source tracking from CEC bus traffic. With the TV known-on, an unknown active source gates (fail safe: a false gate costs one pop gesture, which wakes and re-announces); with no CEC evidence at all, input stays live.
class TvState {
 public:
  void reset() { *this = {}; }
  void set_our_physical_address(std::uint16_t address) { our_physical_address_ = address; }
  std::uint16_t our_physical_address() const { return our_physical_address_; }
  void on_standby();
  void on_power(std::uint8_t cec_status);          // REPORT_POWER_STATUS operand
  void on_active(std::uint16_t physical_address);  // ACTIVE_SOURCE / SET_STREAM_PATH / ROUTING_CHANGE target
  void assume_live();                              // wake-confirmation timeout: fail open until new evidence
  bool is_gated() const;
  bool is_active_source() const {
    return our_physical_address_ != kInvalidPhysicalAddress && active_physical_address_ == our_physical_address_;
  }
  bool has_active_source() const { return active_physical_address_ != kInvalidPhysicalAddress; }

 private:
  enum class Power : std::uint8_t { unknown, on, to_on, standby, to_standby };
  Power power_ = Power::unknown;
  std::uint16_t our_physical_address_ = kInvalidPhysicalAddress;
  std::uint16_t active_physical_address_ = kInvalidPhysicalAddress;
};

struct CecAdapterInfo {
  std::string path;
  std::string name;
};

std::optional<std::string> select_cec_adapter(const std::vector<CecAdapterInfo>& adapters, std::string_view name);
std::optional<std::uint16_t> cec_routing_target(const cec_msg& message);

// The CEC device pair backing the gate: a monitor fd for bus traffic and an initiator/follower fd for transmits and directed messages. With claim_as() set, also owns the adapter's logical-address claim.
class CecLink {
 public:
  // Enables logical-address claiming (playback type, presenting this OSD name); call before open. Negotiation is honest: every (re)claim keeps whatever address the bus yields — other playback devices legitimately hold addresses, and the kernel's cec-claim-disarm patch (machine/k1) keeps our own stale ACK filter from bidding against us. A renegotiation that moves the address while the TV is showing us re-broadcasts ACTIVE_SOURCE from the new address, because the TV routes remote presses by the logical address it last saw claim.
  void claim_as(const std::string& osd_name) { claim_osd_ = osd_name; }
  void set_activate_on_claim(bool activate) { activate_on_claim_ = activate; }
  // Opens both fds and reads the runtime physical address into tv; with claiming enabled, adopts or submits the logical-address claim and puts the tx fd in follower mode.
  bool open_adapter(const std::string& adapter_name, TvState& tv);
  void reset();
  explicit operator bool() const { return static_cast<bool>(mon_) && !failed_; }
  bool failed() const { return failed_; }
  const std::string& device_path() const { return device_path_; }
  int monitor_fd() const { return mon_.get(); }
  int tx_fd() const { return tx_.get(); }
  bool can_send() const { return static_cast<bool>(tx_) && !failed_; }
  bool send(std::uint8_t opcode, std::uint8_t dest, std::initializer_list<std::uint8_t> params);
  // Drains CEC_DQEVENT (physical-address and logical-address changes) and CEC_RECEIVE (bus messages) into tv.
  void drain(std::uint32_t epoll_events, TvState& tv);
  // Drains the follower fd: answers GIVE_DEVICE_POWER_STATUS (always ON) and MENU_REQUEST (always activated; the TV forwards remote presses while the device menu is active), and FEATURE_ABORTs other directed messages — attaching a follower takes that duty from the kernel core, which only aborts unhandled messages itself when no follower exists. Messages consumed elsewhere (monitor fd, kernel rc-cec) are ignored.
  void drain_tx(std::uint32_t epoll_events);
  // Solicits the TV's power status and the bus's active source, once; retried until the adapter has a logical address. The active-source solicitation can draw a stale claim from another source device and gate falsely — accepted: a pop gesture wakes and re-announces, so a false gate costs one pop, while skipping the solicitation would leave input live (blind) with the TV on another source.
  void query_state();
  bool announce_active_source(const TvState& tv);
  // Bus map built from observed REPORT_PHYSICAL_ADDR and other devices' ACTIVE_SOURCE traffic.
  void note_bus_address(std::uint8_t la, std::uint16_t pa);
  int la_for(std::uint16_t pa) const;  // logical address last seen at pa; -1 unknown
  void probe_bus();                    // solicits REPORT_PHYSICAL_ADDR from every remote logical address
  void probe_audio();                  // solicits the audio system's REPORT_PHYSICAL_ADDR unless already mapped
  int audio_la()
      const;  // volume destination per System Audio Control: the audio system when present on the bus, else the TV
  // Broadcasts ACTIVE_SOURCE naming pa. Announcing another device's address is nonstandard (a device announces only itself), but it is the only device-initiated input switch the TV honors.
  bool switch_stream(std::uint16_t pa);

 private:
  bool open_path(const std::string& path, TvState& tv);
  void on_msg(const cec_msg& m, TvState& tv);
  void on_state_change(std::uint16_t phys_addr, std::uint16_t la_mask, TvState& tv);
  void adopt(int la, const TvState& tv);
  bool submit_claim();
  bool is_own_logical_address(std::uint8_t address);
  void reset_protocol_state();
  void note_ioctl_failure();

  unique_fd mon_, tx_;
  std::string device_path_;
  bool state_queried_ = false;
  std::string claim_osd_;
  int la_ = -1;
  bool announced_ = false;
  bool activate_on_claim_ = false;
  bool failed_ = false;
  std::array<std::uint16_t, 15> bus_pa_ = [] {
    std::array<std::uint16_t, 15> a{};
    a.fill(kInvalidPhysicalAddress);
    return a;
  }();
};

}  // namespace evi
