#include <gtest/gtest.h>
#include <linux/cec.h>
#include <linux/input-event-codes.h>
#include <sys/epoll.h>

#include "chord.hpp"
#include "devices.hpp"
#include "gate.hpp"
#include "util.hpp"

namespace evi {
namespace {

TEST(RouteKey, KeysGoToKeyboard) {
  EXPECT_EQ(route_key(KEY_A), Route::kbd);
  EXPECT_EQ(route_key(KEY_ENTER), Route::kbd);
  EXPECT_EQ(route_key(KEY_LEFTMETA), Route::kbd);
  EXPECT_EQ(route_key(KEY_HOMEPAGE), Route::kbd);
  EXPECT_EQ(route_key(KEY_RED), Route::kbd);
  EXPECT_EQ(route_key(KEY_MEDIA_TOP_MENU), Route::kbd);
}

TEST(RouteKey, MouseButtonsGoToPointer) {
  EXPECT_EQ(route_key(BTN_LEFT), Route::pointer);
  EXPECT_EQ(route_key(BTN_RIGHT), Route::pointer);
  EXPECT_EQ(route_key(BTN_MIDDLE), Route::pointer);
  EXPECT_EQ(route_key(BTN_SIDE), Route::pointer);
  EXPECT_EQ(route_key(BTN_EXTRA), Route::pointer);
  EXPECT_EQ(route_key(BTN_TASK), Route::pointer);
}

TEST(RouteKey, PadAndJoystickButtonsDrop) {
  EXPECT_EQ(route_key(BTN_SOUTH), Route::drop);
  EXPECT_EQ(route_key(BTN_MODE), Route::drop);
  EXPECT_EQ(route_key(BTN_START), Route::drop);
  EXPECT_EQ(route_key(BTN_THUMBL), Route::drop);
  EXPECT_EQ(route_key(BTN_TRIGGER), Route::drop);
  EXPECT_EQ(route_key(BTN_DIGI), Route::drop);
  EXPECT_EQ(route_key(BTN_WHEEL), Route::drop);
  EXPECT_EQ(route_key(BTN_DPAD_UP), Route::drop);
  EXPECT_EQ(route_key(BTN_DPAD_RIGHT), Route::drop);
  EXPECT_EQ(route_key(BTN_TRIGGER_HAPPY), Route::drop);
  EXPECT_EQ(route_key(BTN_TRIGGER_HAPPY40), Route::drop);
}

using namespace std::chrono_literals;
constexpr auto t0 = std::chrono::steady_clock::time_point{};

TEST(ChordTracker, DueAfterHoldOnceUntilFiredOrReleased) {
  ChordTracker t({BTN_MODE, BTN_START}, 250ms);
  t.on_key(BTN_MODE, kKeyPress, t0 + 1ms);
  EXPECT_FALSE(t.is_due(t0 + 1ms));
  t.on_key(BTN_START, kKeyPress, t0 + 2ms);
  EXPECT_FALSE(t.is_due(t0 + 2ms));
  EXPECT_FALSE(t.is_due(t0 + 251ms));
  EXPECT_TRUE(t.is_due(t0 + 252ms));
  EXPECT_TRUE(t.is_due(t0 + 252ms));  // pure query: unchanged until marked
  t.mark_fired();
  EXPECT_FALSE(t.is_due(t0 + 500ms));
  t.on_key(BTN_START, kKeyRelease, t0 + 600ms);
  t.on_key(BTN_START, kKeyPress, t0 + 700ms);
  EXPECT_TRUE(t.is_due(t0 + 950ms));
}

TEST(ChordTracker, IgnoresRepeatsAndOtherKeys) {
  ChordTracker t({BTN_MODE, BTN_START}, 250ms);
  t.on_key(BTN_MODE, kKeyPress, t0);
  t.on_key(BTN_START, kKeyPress, t0);
  t.on_key(BTN_SOUTH, kKeyPress, t0);
  t.on_key(BTN_MODE, kKeyAutorepeat, t0 + 100ms);
  EXPECT_TRUE(t.is_due(t0 + 250ms));
}

TEST(ChordTracker, ResetClearsHold) {
  ChordTracker t({BTN_MODE, BTN_START}, 250ms);
  t.on_key(BTN_MODE, kKeyPress, t0);
  t.on_key(BTN_START, kKeyPress, t0);
  t.reset();
  EXPECT_FALSE(t.is_due(t0 + 1s));
}

TEST(ParseChordSpecs, ParsesEntriesWithInstantDefault) {
  auto specs = parse_chord_specs(
      R"([{"buttons": [316, 315], "command": ["/run/pop", "pad chord"]},
          {"buttons": [183], "holdMs": 1500, "command": ["/run/quit"]}])");
  ASSERT_EQ(specs.size(), 2u);
  EXPECT_EQ(specs[0].codes, (std::vector<std::uint16_t>{316, 315}));
  EXPECT_EQ(specs[0].hold, 0ms);
  EXPECT_EQ(specs[0].command, (std::vector<std::string>{"/run/pop", "pad chord"}));
  EXPECT_EQ(specs[1].codes, (std::vector<std::uint16_t>{183}));
  EXPECT_EQ(specs[1].hold, 1500ms);
  EXPECT_THROW(parse_chord_specs("not json"), std::exception);
}

// A single-key zero-hold entry (the forwarded pop key) fires on press, once per press, independently of the pad chord.
TEST(ChordSet, SingleKeyAndChordFireIndependently) {
  ChordSet set({{{KEY_F13}, 0ms, {}}, {{BTN_MODE, BTN_START}, 250ms, {}}});
  set.on_key(KEY_F13, kKeyPress, t0);
  EXPECT_EQ(set.take_due(t0), (std::vector<std::size_t>{0}));
  EXPECT_TRUE(set.take_due(t0 + 1s).empty());
  set.on_key(KEY_F13, kKeyRelease, t0 + 1s);
  set.on_key(KEY_F13, kKeyPress, t0 + 2s);
  EXPECT_EQ(set.take_due(t0 + 2s), (std::vector<std::size_t>{0}));
  set.on_key(BTN_MODE, kKeyPress, t0 + 3s);
  set.on_key(BTN_START, kKeyPress, t0 + 3s);
  EXPECT_TRUE(set.take_due(t0 + 3s + 100ms).empty());
  EXPECT_EQ(set.take_due(t0 + 3s + 300ms), (std::vector<std::size_t>{1}));
}

TEST(AbsRest, DeliberateDeflection) {
  AbsRest stick{0, 8192};  // centered, range -32768..32767
  EXPECT_FALSE(is_deliberate(stick, 4000));
  EXPECT_FALSE(is_deliberate(stick, -8192));
  EXPECT_TRUE(is_deliberate(stick, -20000));
  AbsRest trigger{0, 63};  // rests at its minimum, range 0..255
  EXPECT_FALSE(is_deliberate(trigger, 40));
  EXPECT_TRUE(is_deliberate(trigger, 200));
  AbsRest hat{0, 0};  // digital, range -1..1
  EXPECT_FALSE(is_deliberate(hat, 0));
  EXPECT_TRUE(is_deliberate(hat, 1));
  EXPECT_TRUE(is_deliberate(hat, -1));
}

TEST(CecAdapter, SelectsByNameInsteadOfNodeNumber) {
  std::vector<CecAdapterInfo> adapters = {
      {"/dev/cec0", "HDMI-A-1"},
      {"/dev/cec7", "DP-2"},
  };
  EXPECT_EQ(select_cec_adapter(adapters, "DP-2"), std::optional<std::string>("/dev/cec7"));
  EXPECT_EQ(select_cec_adapter(adapters, "DP-3"), std::nullopt);
}

TEST(CecRouting, ExtractsEveryActiveTargetForm) {
  cec_msg active{};
  active.len = 4;
  active.msg[1] = CEC_MSG_ACTIVE_SOURCE;
  active.msg[2] = 0x20;
  active.msg[3] = 0x00;
  EXPECT_EQ(cec_routing_target(active), 0x2000);

  cec_msg information{};
  information.len = 4;
  information.msg[1] = CEC_MSG_ROUTING_INFORMATION;
  information.msg[2] = 0x10;
  information.msg[3] = 0x00;
  EXPECT_EQ(cec_routing_target(information), 0x1000);

  cec_msg change{};
  change.len = 6;
  change.msg[1] = CEC_MSG_ROUTING_CHANGE;
  change.msg[2] = 0x10;
  change.msg[3] = 0x00;
  change.msg[4] = 0x20;
  change.msg[5] = 0x00;
  EXPECT_EQ(cec_routing_target(change), 0x2000);

  cec_msg stream{};
  stream.len = 4;
  stream.msg[1] = CEC_MSG_SET_STREAM_PATH;
  stream.msg[2] = 0x20;
  stream.msg[3] = 0x00;
  EXPECT_EQ(cec_routing_target(stream), 0x2000);
}

TEST(CecRouting, RejectsTruncatedMessages) {
  cec_msg message{};
  message.len = 3;
  message.msg[1] = CEC_MSG_ROUTING_INFORMATION;
  EXPECT_EQ(cec_routing_target(message), std::nullopt);
}

TEST(CecLink, ErrorEventsDisconnectUntilReset) {
  CecLink link;
  TvState tv;
  link.drain(EPOLLHUP, tv);
  EXPECT_TRUE(link.failed());
  link.reset();
  EXPECT_FALSE(link.failed());
  link.drain_tx(EPOLLERR);
  EXPECT_TRUE(link.failed());
}

TEST(TvState, DefaultsLive) {
  TvState tv;
  EXPECT_FALSE(tv.is_gated());
  tv.set_our_physical_address(0x2000);
  EXPECT_FALSE(tv.is_gated());
}

TEST(TvState, GatesUnlessOn) {
  TvState tv;
  tv.on_standby();
  EXPECT_TRUE(tv.is_gated());
  tv.on_power(CEC_OP_POWER_STATUS_ON);
  EXPECT_FALSE(tv.is_gated());
  tv.on_power(CEC_OP_POWER_STATUS_STANDBY);
  EXPECT_TRUE(tv.is_gated());
  tv.on_power(CEC_OP_POWER_STATUS_TO_STANDBY);
  EXPECT_TRUE(tv.is_gated());
  tv.on_power(CEC_OP_POWER_STATUS_TO_ON);
  EXPECT_TRUE(tv.is_gated());  // no picture yet
  tv.on_power(CEC_OP_POWER_STATUS_ON);
  EXPECT_FALSE(tv.is_gated());
}

TEST(TvState, OnWithUnknownActiveSourceGates) {
  TvState tv;
  tv.set_our_physical_address(0x2000);
  tv.on_power(CEC_OP_POWER_STATUS_ON);
  EXPECT_TRUE(tv.is_gated());  // non-CEC inputs answer nothing; fail safe
  tv.on_active(0x2000);
  EXPECT_FALSE(tv.is_gated());
}

TEST(TvState, OtherActiveSourceGates) {
  TvState tv;
  tv.set_our_physical_address(0x2000);
  tv.on_active(0x1000);
  EXPECT_TRUE(tv.is_gated());
  tv.on_active(0x2000);
  EXPECT_FALSE(tv.is_gated());
}

// Claim re-assertion (SET_STREAM_PATH confirm, REQUEST_ACTIVE_SOURCE answer) keys off this: only when the bus evidence says the active source is us.
TEST(TvState, IsActiveSourceRequiresOwnAddressMatch) {
  TvState tv;
  EXPECT_FALSE(tv.is_active_source());
  tv.on_active(0x2000);
  EXPECT_FALSE(tv.is_active_source());  // our own address unknown: nothing to claim
  tv.set_our_physical_address(0x2000);
  EXPECT_TRUE(tv.is_active_source());
  tv.on_active(0x1000);
  EXPECT_FALSE(tv.is_active_source());
}

TEST(TvState, ActiveMismatchWithoutOwnPhysicalAddressFailsOpen) {
  TvState tv;
  tv.on_active(0x1000);
  EXPECT_FALSE(tv.is_gated());
}

TEST(TvState, AssumeLiveOverridesEvidence) {
  TvState tv;
  tv.set_our_physical_address(0x2000);
  tv.on_standby();
  tv.on_active(0x1000);
  EXPECT_TRUE(tv.is_gated());
  tv.assume_live();
  EXPECT_FALSE(tv.is_gated());
  tv.on_standby();  // new evidence re-gates
  EXPECT_TRUE(tv.is_gated());
}

TEST(TvState, ResetDropsConnectionEvidence) {
  TvState tv;
  tv.set_our_physical_address(0x2000);
  tv.on_power(CEC_OP_POWER_STATUS_ON);
  tv.on_active(0x1000);
  EXPECT_TRUE(tv.is_gated());
  tv.reset();
  EXPECT_FALSE(tv.has_active_source());
  EXPECT_FALSE(tv.is_gated());
}

}  // namespace
}  // namespace evi
