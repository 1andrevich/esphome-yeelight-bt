#pragma once

#ifdef USE_ESP32

#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#include "yeelight_bt.h"

namespace esphome {
namespace yeelight_bt {

/// Send CMD_PAIR. Put this behind a `button:` so the user can pair without
/// reflashing.
template<typename... Ts> class PairAction : public Action<Ts...>, public Parented<YeelightBTLight> {
 public:
  void play(Ts... x) override { this->parent_->pair(); }
};

/// Ask the lamp for its current state on demand.
template<typename... Ts> class RequestStateAction : public Action<Ts...>, public Parented<YeelightBTLight> {
 public:
  void play(Ts... x) override { this->parent_->request_state(); }
};

/// Write an arbitrary frame. For protocol exploration from test.yaml -- see
/// PROTOCOL.md for the open questions this is meant to answer.
template<typename... Ts> class SendRawAction : public Action<Ts...>, public Parented<YeelightBTLight> {
 public:
  void set_data(const std::vector<uint8_t> &data) { this->data_ = data; }
  void play(Ts... x) override { this->parent_->send_raw(this->data_); }

 protected:
  std::vector<uint8_t> data_;
};

/// Fires once the lamp reports it is paired (or that pairing does not apply).
class PairedTrigger : public Trigger<> {
 public:
  explicit PairedTrigger(YeelightBTLight *parent) {
    parent->add_paired_callback([this]() { this->trigger(); });
  }
};

}  // namespace yeelight_bt
}  // namespace esphome

#endif  // USE_ESP32
