#pragma once

#ifdef USE_ESP32

#include <array>
#include <deque>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/light/light_traits.h"

namespace esphome {
namespace yeelight_bt {

namespace espbt = esphome::esp32_ble_tracker;

/// Every request and response is exactly 18 bytes, zero padded.
static const uint8_t FRAME_SIZE = 18;

using Frame = std::array<uint8_t, FRAME_SIZE>;

// ---------------------------------------------------------------------------
// Protocol constants.
//
// Verified against custom_components/yeelight_bt/yeelightbt.py in
// hcoohb/hass-yeelightbt, the maintained Python implementation used as the
// reference. Where it disagrees with the older reverse-engineering notes
// (Marcocanc/mi-lamp-re) the Python wins, because it is the implementation
// known to work against current hardware. See PROTOCOL.md.
// ---------------------------------------------------------------------------

static const uint8_t COMMAND_STX = 0x43;

static const uint8_t CMD_PAIR = 0x67;
/// The pairing payload is this single constant byte -- NOT a 16-byte client
/// UUID -- the pairing lives in the lamp, so there is nothing to persist.
/// See PROTOCOL.md.
static const uint8_t CMD_PAIR_ON = 0x02;
static const uint8_t CMD_POWER = 0x40;
static const uint8_t CMD_POWER_ON = 0x01;
static const uint8_t CMD_POWER_OFF = 0x02;
static const uint8_t CMD_RGB = 0x41;
static const uint8_t CMD_BRIGHTNESS = 0x42;
static const uint8_t CMD_TEMP = 0x43;
static const uint8_t CMD_GETSTATE = 0x44;
/// CMD_GETSTATE carries a payload byte; it is not an empty request.
static const uint8_t CMD_GETSTATE_SEC = 0x02;
static const uint8_t CMD_GETNAME = 0x52;
static const uint8_t CMD_GETVER = 0x5C;
static const uint8_t CMD_GETSERIAL = 0x5E;

static const uint8_t RES_PAIR = 0x63;
static const uint8_t RES_GETSTATE = 0x45;
static const uint8_t RES_GETNAME = 0x53;
static const uint8_t RES_GETVER = 0x5D;
static const uint8_t RES_GETSERIAL = 0x5F;

// Pairing response codes, byte [2] of a RES_PAIR notification.
static const uint8_t PAIR_REQUESTED = 0x01;  ///< Lamp pulses, user must press its button.
static const uint8_t PAIR_SUCCESS = 0x02;
static const uint8_t PAIR_UNPAIRED = 0x03;
static const uint8_t PAIR_ALREADY = 0x04;  ///< Already paired; no button press needed.
static const uint8_t PAIR_ERROR_6 = 0x06;
static const uint8_t PAIR_ERROR_7 = 0x07;

// Lamp operating mode, byte [3] of a bedside RES_GETSTATE.
static const uint8_t MODE_COLOR = 0x01;
static const uint8_t MODE_WHITE = 0x02;
static const uint8_t MODE_FLOW = 0x03;

static const uint16_t TEMP_MIN_KELVIN = 1700;
static const uint16_t TEMP_MAX_KELVIN = 6500;

/// Ignore incoming state for this long after our own write, so the lamp
/// echoing back what we just told it does not fight the frontend.
static const uint32_t STATE_ECHO_GUARD_MS = 500;

/// How long a pending "this came from the lamp" marker stays valid before it
/// is treated as stale and a write is allowed through.
static const uint32_t SUPPRESS_WRITE_TIMEOUT_MS = 2000;

/// Bound the outbound queue. A dragged slider can otherwise produce frames
/// faster than the drain timer retires them.
static const size_t MAX_QUEUE_DEPTH = 12;

/// Consecutive failed opens before we name the single-connection cause.
static const uint8_t OPEN_FAILURE_HINT_THRESHOLD = 3;

enum class Model : uint8_t {
  BEDSIDE = 0,
  CANDELA = 1,
};

enum class PairState : uint8_t {
  DISCONNECTED = 0,
  UNPAIRED,
  PAIRING,
  PAIRED,
  PAIRING_UNSUPPORTED,
};

const char *pair_state_to_string(PairState state);

class YeelightBTLight : public light::LightOutput, public Component, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  // light::LightOutput
  light::LightTraits get_traits() override;
  void setup_state(light::LightState *state) override;
  void write_state(light::LightState *state) override;

  void set_model(Model model) { this->model_ = model; }
  void set_poll_interval(uint32_t poll_interval) { this->poll_interval_ = poll_interval; }
  void set_write_interval(uint32_t write_interval) { this->write_interval_ = write_interval; }

  /// Send CMD_PAIR. Safe to call at any time; the lamp answers 0x04 when it is
  /// already paired, so this does not force a button press on every boot.
  void pair();
  /// Ask the lamp for its current state.
  void request_state();
  /// Ask for firmware version / serial. Answers land in the log.
  void request_version();
  void request_name();
  void request_serial();
  /// Escape hatch for protocol exploration from YAML. Frames shorter than 18
  /// bytes are zero padded, longer ones rejected.
  void send_raw(const std::vector<uint8_t> &data);

  void add_paired_callback(std::function<void()> &&callback) { this->paired_callback_.add(std::move(callback)); }

  PairState get_pair_state() const { return this->pair_state_; }

  /// True once the GATT link is up and notifications are subscribed.
  bool is_connected() const { return this->node_state == espbt::ClientState::ESTABLISHED; }

  /// Raw payload of the last RES_GETVER, empty until the lamp answers.
  const std::string &get_firmware_version() const { return this->firmware_version_; }

 protected:
  Frame make_frame_(uint8_t command, std::initializer_list<uint8_t> payload);
  void enqueue_(const Frame &frame);
  bool write_frame_(const Frame &frame);

  void handle_notify_(const uint8_t *data, uint16_t length);
  void parse_state_(const uint8_t *data);
  void handle_pair_response_(uint8_t code);
  void publish_remote_state_(bool on, uint8_t brightness);
  void reset_connection_state_();
  bool ready_for_commands_() const;

  static uint8_t scale_brightness_(float brightness);

  Model model_{Model::BEDSIDE};
  uint32_t poll_interval_{30000};
  uint32_t write_interval_{80};

  light::LightState *light_state_{nullptr};

  /// Set when we push lamp-originated state into the frontend, and consumed by
  /// the next write_state(), so that state does not bounce straight back at
  /// the lamp.
  ///
  /// This cannot be a plain "am I inside perform()" flag: LightCall::perform()
  /// does not call write_state() synchronously, it sets next_write_ and
  /// LightState::loop() calls us on a later iteration. Expiry stops a stale
  /// flag from swallowing a genuine user command if that call never arrives.
  bool suppress_next_write_{false};
  uint32_t suppress_set_at_{0};

  uint16_t control_handle_{0};
  uint16_t notify_handle_{0};
  uint8_t open_failures_{0};

  PairState pair_state_{PairState::DISCONNECTED};

  std::deque<Frame> tx_queue_;
  uint32_t last_write_{0};

  // Last state reported by the lamp.
  bool lamp_on_{false};
  uint8_t lamp_mode_{0};
  uint8_t lamp_red_{0};
  uint8_t lamp_green_{0};
  uint8_t lamp_blue_{0};
  /// Byte [7] of RES_GETSTATE. Purpose unknown; logged so it can be identified.
  uint8_t lamp_unknown_byte_{0};
  uint8_t lamp_brightness_{0};
  uint16_t lamp_temperature_{0};

  std::string firmware_version_;
  std::string lamp_name_;
  std::string serial_;

  CallbackManager<void()> paired_callback_;
};

}  // namespace yeelight_bt
}  // namespace esphome

// Pulled in at the bottom, after YeelightBTLight is complete, so the action and
// trigger classes are always visible to generated code regardless of which of
// the two headers is included first. Both use #pragma once, so the reverse
// include order resolves correctly too.
#include "automation.h"

#endif  // USE_ESP32
