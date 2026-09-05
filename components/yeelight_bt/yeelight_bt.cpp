#include "yeelight_bt.h"

#ifdef USE_ESP32

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace yeelight_bt {

static const char *const TAG = "yeelight_bt";

// The Python reference writes by characteristic UUID and never names a service.
// ESPHome resolves characteristics through their service, so the service UUID
// comes from the reverse-engineering notes (Marcocanc/mi-lamp-re).
static const espbt::ESPBTUUID SERVICE_UUID = espbt::ESPBTUUID::from_raw("8e2f0cbd-1a66-4b53-ace6-b494e25f87bd");
static const espbt::ESPBTUUID CONTROL_UUID = espbt::ESPBTUUID::from_raw("aa7d3f34-2d4f-41e0-807f-52fbf8cf7443");
static const espbt::ESPBTUUID NOTIFY_UUID = espbt::ESPBTUUID::from_raw("8f65073d-9f57-4aaa-afea-397d19d5bbeb");

const char *pair_state_to_string(PairState state) {
  switch (state) {
    case PairState::DISCONNECTED:
      return "DISCONNECTED";
    case PairState::UNPAIRED:
      return "UNPAIRED";
    case PairState::PAIRING:
      return "PAIRING";
    case PairState::PAIRED:
      return "PAIRED";
    case PairState::PAIRING_UNSUPPORTED:
      return "PAIRING_UNSUPPORTED";
  }
  return "UNKNOWN";
}

void YeelightBTLight::setup() {
  // Nothing to do until ble_client connects. Register the state poll only if
  // the user asked for one; 0 means never poll.
  if (this->poll_interval_ > 0) {
    this->set_interval("poll", this->poll_interval_, [this]() {
      if (this->ready_for_commands_())
        this->request_state();
    });
  }
  this->status_set_warning("not connected");
}

void YeelightBTLight::loop() {
  if (this->node_state != espbt::ClientState::ESTABLISHED) {
    if (!this->tx_queue_.empty()) {
      ESP_LOGD(TAG, "Dropping %u queued frame(s), link is down", (unsigned) this->tx_queue_.size());
      this->tx_queue_.clear();
    }
    return;
  }

  if (this->tx_queue_.empty())
    return;

  const uint32_t now = millis();
  if (now - this->last_write_ < this->write_interval_)
    return;

  Frame frame = this->tx_queue_.front();
  this->tx_queue_.pop_front();
  this->write_frame_(frame);
  this->last_write_ = now;
}

// ---------------------------------------------------------------------------
// Frame construction and the outbound queue
// ---------------------------------------------------------------------------

Frame YeelightBTLight::make_frame_(uint8_t command, std::initializer_list<uint8_t> payload) {
  Frame frame{};  // value-initialised: zero padded
  frame[0] = COMMAND_STX;
  frame[1] = command;
  size_t i = 2;
  for (uint8_t byte : payload) {
    if (i >= FRAME_SIZE)
      break;
    frame[i++] = byte;
  }
  return frame;
}

void YeelightBTLight::enqueue_(const Frame &frame) {
  // Coalesce: a queued frame carrying the same command byte is superseded by
  // the newer one, in place, so a dragged slider collapses to one write rather
  // than flooding the lamp's GATT stack.
  for (auto &queued : this->tx_queue_) {
    if (queued[1] == frame[1]) {
      queued = frame;
      return;
    }
  }

  if (this->tx_queue_.size() >= MAX_QUEUE_DEPTH) {
    ESP_LOGW(TAG, "Outbound queue full, dropping oldest frame");
    this->tx_queue_.pop_front();
  }
  this->tx_queue_.push_back(frame);
}

bool YeelightBTLight::write_frame_(const Frame &frame) {
  if (this->control_handle_ == 0) {
    ESP_LOGW(TAG, "No control handle, cannot write");
    return false;
  }

  Frame payload = frame;  // esp_ble_gattc_write_char wants a non-const buffer
  auto status = esp_ble_gattc_write_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(),
                                         this->control_handle_, (uint16_t) payload.size(), payload.data(),
                                         ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "esp_ble_gattc_write_char failed, status=%d", status);
    return false;
  }

  ESP_LOGV(TAG, "TX %s", format_hex_pretty(payload.data(), payload.size()).c_str());
  return true;
}

bool YeelightBTLight::ready_for_commands_() const {
  if (this->node_state != espbt::ClientState::ESTABLISHED)
    return false;
  return this->pair_state_ == PairState::PAIRED || this->pair_state_ == PairState::PAIRING_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void YeelightBTLight::pair() {
  if (this->node_state != espbt::ClientState::ESTABLISHED) {
    ESP_LOGW(TAG, "Cannot pair, not connected");
    return;
  }
  ESP_LOGI(TAG, "Sending pair request");
  this->enqueue_(this->make_frame_(CMD_PAIR, {CMD_PAIR_ON}));
}

void YeelightBTLight::request_state() {
  if (this->node_state != espbt::ClientState::ESTABLISHED)
    return;
  this->enqueue_(this->make_frame_(CMD_GETSTATE, {CMD_GETSTATE_SEC}));
}

void YeelightBTLight::request_version() { this->enqueue_(this->make_frame_(CMD_GETVER, {})); }

void YeelightBTLight::request_name() { this->enqueue_(this->make_frame_(CMD_GETNAME, {})); }

void YeelightBTLight::request_serial() { this->enqueue_(this->make_frame_(CMD_GETSERIAL, {})); }

void YeelightBTLight::send_raw(const std::vector<uint8_t> &data) {
  if (data.empty()) {
    ESP_LOGW(TAG, "send_raw called with no data");
    return;
  }
  if (data.size() > FRAME_SIZE) {
    ESP_LOGW(TAG, "send_raw frame too long (%u bytes, max %u)", (unsigned) data.size(), (unsigned) FRAME_SIZE);
    return;
  }
  if (this->node_state != espbt::ClientState::ESTABLISHED) {
    ESP_LOGW(TAG, "send_raw dropped, not connected");
    return;
  }

  Frame frame{};
  std::memcpy(frame.data(), data.data(), data.size());
  ESP_LOGD(TAG, "send_raw %s", format_hex_pretty(frame.data(), frame.size()).c_str());
  // Deliberately bypasses coalescing: raw frames are for protocol exploration
  // and must go out exactly as written, in order. The depth bound still
  // applies.
  if (this->tx_queue_.size() >= MAX_QUEUE_DEPTH) {
    ESP_LOGW(TAG, "Outbound queue full, dropping oldest frame");
    this->tx_queue_.pop_front();
  }
  this->tx_queue_.push_back(frame);
}

// ---------------------------------------------------------------------------
// GATT events
// ---------------------------------------------------------------------------

void YeelightBTLight::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                          esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "[%s] Connected", this->parent_->address_str());
        this->open_failures_ = 0;
      } else {
        this->open_failures_++;
        ESP_LOGW(TAG, "[%s] Connection failed, status=%d (attempt %u)", this->parent_->address_str(),
                 (int) param->open.status, (unsigned) this->open_failures_);
        if (this->open_failures_ == OPEN_FAILURE_HINT_THRESHOLD) {
          ESP_LOGE(TAG,
                   "Repeated connection failures. This lamp accepts exactly ONE BLE connection at a time. "
                   "Close the Yeelight phone app completely, including from the background, and make sure no "
                   "other ESP node or BLE proxy holds the lamp.");
        }
      }
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *control = this->parent_->get_characteristic(SERVICE_UUID, CONTROL_UUID);
      auto *notify = this->parent_->get_characteristic(SERVICE_UUID, NOTIFY_UUID);

      if (control == nullptr || notify == nullptr) {
        ESP_LOGE(TAG,
                 "[%s] Yeelight service/characteristics not found -- is this really a BLE Yeelight lamp? "
                 "control=%s notify=%s",
                 this->parent_->address_str(), control == nullptr ? "missing" : "ok",
                 notify == nullptr ? "missing" : "ok");
        // Give up cleanly rather than crashing; ble_client will retry the link.
        break;
      }

      this->control_handle_ = control->handle;
      this->notify_handle_ = notify->handle;
      ESP_LOGD(TAG, "Resolved handles: control=0x%04X notify=0x%04X", this->control_handle_, this->notify_handle_);

      // Register for notifications and wait for ESP_GATTC_REG_FOR_NOTIFY_EVT
      // before sending anything. Writing before the subscription completes is
      // the classic silent failure here. ESP-IDF writes the CCCD itself as
      // part of this call, so we do not write the descriptor by hand -- this
      // matches how the in-tree anova and am43 components do it.
      auto status = esp_ble_gattc_register_for_notify(this->parent_->get_gattc_if(), this->parent_->get_remote_bda(),
                                                      notify->handle);
      if (status != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gattc_register_for_notify failed, status=%d", status);
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Notify registration failed, status=%d", (int) param->reg_for_notify.status);
        break;
      }
      this->node_state = espbt::ClientState::ESTABLISHED;
      this->pair_state_ = PairState::UNPAIRED;
      this->status_clear_warning();
      ESP_LOGD(TAG, "Notifications established");

      // Pairing is idempotent: an already-paired lamp answers PAIR_ALREADY
      // without asking for a button press, so this is safe on every connect.
      this->pair();
      this->request_version();
      this->request_state();
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->notify_handle_)
        break;
      this->handle_notify_(param->notify.value, param->notify.value_len);
      break;
    }

    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "[%s] Disconnected", this->parent_->address_str());
      this->reset_connection_state_();
      break;
    }

    default:
      break;
  }
}

void YeelightBTLight::reset_connection_state_() {
  this->control_handle_ = 0;
  this->notify_handle_ = 0;
  this->pair_state_ = PairState::DISCONNECTED;
  this->node_state = espbt::ClientState::IDLE;
  this->tx_queue_.clear();
  // ESPHome's native API has no per-entity availability flag the way MQTT
  // does; a component warning status is the idiomatic equivalent and is what
  // surfaces to a client.
  this->status_set_warning("disconnected");
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

void YeelightBTLight::handle_notify_(const uint8_t *data, uint16_t length) {
  ESP_LOGV(TAG, "RX %s", format_hex_pretty(data, length).c_str());

  if (length < FRAME_SIZE) {
    ESP_LOGW(TAG, "Short notification (%u bytes, expected %u)", (unsigned) length, (unsigned) FRAME_SIZE);
    return;
  }
  if (data[0] != COMMAND_STX) {
    ESP_LOGW(TAG, "Unexpected magic byte 0x%02X", data[0]);
    return;
  }

  switch (data[1]) {
    case RES_GETSTATE:
      this->parse_state_(data);
      break;

    case RES_PAIR:
      this->handle_pair_response_(data[2]);
      break;

    case RES_GETVER: {
      // The Python reference unpacks this as ">xxBHHHH6x" and then casts the
      // resulting tuple straight to str, which is almost certainly a bug there
      // -- so the real field layout is not actually established. Keep the raw
      // payload rather than inventing a version format. See PROTOCOL.md.
      this->firmware_version_ = format_hex_pretty(data + 2, FRAME_SIZE - 2);
      ESP_LOGI(TAG, "Firmware version (raw payload): %s", this->firmware_version_.c_str());
      break;
    }

    case RES_GETNAME: {
      // ASCII name in the payload, not necessarily NUL terminated.
      char buf[FRAME_SIZE - 1] = {};
      std::memcpy(buf, data + 2, FRAME_SIZE - 2);
      this->lamp_name_ = buf;
      ESP_LOGI(TAG, "Lamp name: %s", this->lamp_name_.c_str());
      break;
    }

    case RES_GETSERIAL: {
      this->serial_ = format_hex_pretty(data + 2, FRAME_SIZE - 2);
      ESP_LOGI(TAG, "Serial (raw payload): %s", this->serial_.c_str());
      break;
    }

    default:
      ESP_LOGD(TAG, "Unhandled response type 0x%02X", data[1]);
      break;
  }
}

void YeelightBTLight::parse_state_(const uint8_t *data) {
  bool on;
  uint8_t brightness;

  if (this->model_ == Model::CANDELA) {
    // Candela reports a different, shorter layout. Untested on hardware.
    on = data[2] == CMD_POWER_ON;
    brightness = data[3];
    this->lamp_mode_ = data[4];
  } else {
    // Bedside layout, per the Python reference's
    // struct.unpack(">xxBBBBBBBhx6x", data) and its field assignment:
    //   [2]=power [3]=mode [4]=R [5]=G [6]=B [7]=unused [8]=brightness
    //   [9..10]=temperature, big endian
    //
    // NOTE: the reverse-engineering notes place RGB at [5][6][7], which reads
    // (green, blue, unknown) as the colour. [4] is the red channel and [7] is
    // the byte whose purpose is unknown. See PROTOCOL.md.
    on = data[2] == CMD_POWER_ON;
    this->lamp_mode_ = data[3];
    this->lamp_red_ = data[4];
    this->lamp_green_ = data[5];
    this->lamp_blue_ = data[6];
    this->lamp_unknown_byte_ = data[7];
    brightness = data[8];
    this->lamp_temperature_ = (uint16_t) ((uint16_t) data[9] << 8) | (uint16_t) data[10];
  }

  this->lamp_on_ = on;
  this->lamp_brightness_ = brightness;

  ESP_LOGD(TAG, "State: on=%s mode=0x%02X rgb=(%u,%u,%u) byte7=0x%02X brightness=%u temp=%uK", YESNO(on),
           this->lamp_mode_, this->lamp_red_, this->lamp_green_, this->lamp_blue_, this->lamp_unknown_byte_,
           brightness, this->lamp_temperature_);

  // Guard against feedback loops: the lamp echoes state right after our own
  // write, and re-publishing that would fight whatever the user is dragging.
  if (millis() - this->last_write_ < STATE_ECHO_GUARD_MS) {
    ESP_LOGV(TAG, "Ignoring state echo within guard window");
    return;
  }

  this->publish_remote_state_(on, brightness);
}

void YeelightBTLight::handle_pair_response_(uint8_t code) {
  switch (code) {
    case PAIR_REQUESTED:
      this->pair_state_ = PairState::PAIRING;
      ESP_LOGW(TAG, "Lamp is pulsing and waiting for you to PRESS ITS BUTTON to confirm pairing");
      break;

    case PAIR_SUCCESS:
      this->pair_state_ = PairState::PAIRED;
      ESP_LOGI(TAG, "Paired");
      this->paired_callback_.call();
      this->request_state();
      break;

    case PAIR_ALREADY:
      this->pair_state_ = PairState::PAIRED;
      ESP_LOGI(TAG, "Already paired");
      this->paired_callback_.call();
      this->request_state();
      break;

    case PAIR_UNPAIRED:
      this->pair_state_ = PairState::UNPAIRED;
      ESP_LOGW(TAG, "Lamp reports UNPAIRED. Call the yeelight_bt.pair action and press the lamp's button.");
      break;

    case PAIR_ERROR_6:
    case PAIR_ERROR_7:
      // The plan claims 0x07 means "pairing not supported" on Candela. The
      // Python reference treats 0x06/0x07 alike as an error requiring a reset.
      // We follow the Python, but on Candela we let commands through anyway
      // since that model reportedly never completes a pairing handshake.
      if (this->model_ == Model::CANDELA) {
        this->pair_state_ = PairState::PAIRING_UNSUPPORTED;
        ESP_LOGW(TAG, "Pairing not supported on this model (code 0x%02X); proceeding without it", code);
        this->paired_callback_.call();
        this->request_state();
      } else {
        this->pair_state_ = PairState::UNPAIRED;
        ESP_LOGE(TAG,
                 "Pairing error 0x%02X. The lamp may need a factory reset "
                 "(hold its button until it blinks) before it will pair again.",
                 code);
      }
      break;

    default:
      ESP_LOGW(TAG, "Unknown pairing response 0x%02X", code);
      break;
  }
}

// ---------------------------------------------------------------------------
// Light platform
// ---------------------------------------------------------------------------

light::LightTraits YeelightBTLight::get_traits() {
  auto traits = light::LightTraits();
  if (this->model_ == Model::CANDELA) {
    traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
  } else {
    traits.set_supported_color_modes({light::ColorMode::RGB, light::ColorMode::COLOR_TEMPERATURE});
    // 1000000 / 6500K = 153 mireds, 1000000 / 1700K = 588 mireds
    traits.set_min_mireds(153.0f);
    traits.set_max_mireds(588.0f);
  }
  return traits;
}

void YeelightBTLight::setup_state(light::LightState *state) { this->light_state_ = state; }

uint8_t YeelightBTLight::scale_brightness_(float brightness) {
  // The lamp takes 0-100, not 0-255, and 0 is not a valid brightness.
  int scaled = (int) roundf(brightness * 100.0f);
  return (uint8_t) clamp(scaled, 1, 100);
}

void YeelightBTLight::write_state(light::LightState *state) {
  // This change originated at the lamp, so do not send it back.
  if (this->suppress_next_write_) {
    this->suppress_next_write_ = false;
    if (millis() - this->suppress_set_at_ < SUPPRESS_WRITE_TIMEOUT_MS) {
      ESP_LOGV(TAG, "Suppressing echo of lamp-originated state");
      return;
    }
  }

  if (!this->ready_for_commands_()) {
    ESP_LOGW(TAG, "Dropping light command: link %s, pairing %s",
             this->node_state == espbt::ClientState::ESTABLISHED ? "up" : "down",
             pair_state_to_string(this->pair_state_));
    return;
  }

  auto values = state->current_values;

  if (!values.is_on()) {
    // Clear first so a queued colour frame cannot arrive after the off and
    // wake the lamp back up.
    this->tx_queue_.clear();
    this->enqueue_(this->make_frame_(CMD_POWER, {CMD_POWER_OFF}));
    this->lamp_on_ = false;
    return;
  }

  if (!this->lamp_on_) {
    this->enqueue_(this->make_frame_(CMD_POWER, {CMD_POWER_ON}));
    this->lamp_on_ = true;
  }

  const uint8_t brightness = scale_brightness_(values.get_brightness());

  switch (values.get_color_mode()) {
    case light::ColorMode::COLOR_TEMPERATURE: {
      const float mireds = clamp(values.get_color_temperature(), 153.0f, 588.0f);
      uint16_t kelvin = (uint16_t) clamp((int) roundf(1000000.0f / mireds), (int) TEMP_MIN_KELVIN,
                                         (int) TEMP_MAX_KELVIN);
      // The lamp switches out of colour mode on its own when it receives a
      // temperature write, so no explicit mode change is needed first.
      this->enqueue_(this->make_frame_(CMD_TEMP, {(uint8_t) (kelvin >> 8), (uint8_t) (kelvin & 0xFF), brightness}));
      break;
    }

    case light::ColorMode::RGB: {
      // CMD_RGB carries brightness in its own payload, so an RGB change is a
      // single write -- no separate CMD_BRIGHTNESS is required.
      const uint8_t red = (uint8_t) clamp((int) roundf(values.get_red() * 255.0f), 0, 255);
      const uint8_t green = (uint8_t) clamp((int) roundf(values.get_green() * 255.0f), 0, 255);
      const uint8_t blue = (uint8_t) clamp((int) roundf(values.get_blue() * 255.0f), 0, 255);
      this->enqueue_(this->make_frame_(CMD_RGB, {red, green, blue, 0x01, brightness}));
      break;
    }

    default:
      this->enqueue_(this->make_frame_(CMD_BRIGHTNESS, {brightness}));
      break;
  }
}

void YeelightBTLight::publish_remote_state_(bool on, uint8_t brightness) {
  if (this->light_state_ == nullptr)
    return;

  this->suppress_next_write_ = true;
  this->suppress_set_at_ = millis();

  auto call = this->light_state_->make_call();
  call.set_state(on);
  call.set_transition_length((uint32_t) 0);

  if (on) {
    call.set_brightness(clamp((float) brightness / 100.0f, 0.01f, 1.0f));

    if (this->model_ != Model::CANDELA) {
      if (this->lamp_mode_ == MODE_COLOR) {
        call.set_color_mode(light::ColorMode::RGB);
        call.set_rgb((float) this->lamp_red_ / 255.0f, (float) this->lamp_green_ / 255.0f,
                     (float) this->lamp_blue_ / 255.0f);
      } else if (this->lamp_mode_ == MODE_WHITE && this->lamp_temperature_ > 0) {
        const uint16_t kelvin = clamp(this->lamp_temperature_, TEMP_MIN_KELVIN, TEMP_MAX_KELVIN);
        call.set_color_mode(light::ColorMode::COLOR_TEMPERATURE);
        call.set_color_temperature(1000000.0f / (float) kelvin);
      } else if (this->lamp_mode_ == MODE_FLOW) {
        ESP_LOGD(TAG, "Lamp is in flow mode; publishing power and brightness only");
      }
    }
  }

  // perform() does not call write_state() here and now; it flags LightState,
  // which calls us back on a later loop iteration. suppress_next_write_ is
  // consumed there.
  call.perform();
}

void YeelightBTLight::dump_config() {
  ESP_LOGCONFIG(TAG, "Yeelight BT Light:");
  ESP_LOGCONFIG(TAG, "  MAC address: %s", this->parent_ != nullptr ? this->parent_->address_str() : "unknown");
  ESP_LOGCONFIG(TAG, "  Model: %s", this->model_ == Model::CANDELA ? "candela" : "bedside");
  ESP_LOGCONFIG(TAG, "  Pairing state: %s", pair_state_to_string(this->pair_state_));
  ESP_LOGCONFIG(TAG, "  Poll interval: %u ms%s", (unsigned) this->poll_interval_,
                this->poll_interval_ == 0 ? " (polling disabled)" : "");
  ESP_LOGCONFIG(TAG, "  Write interval: %u ms", (unsigned) this->write_interval_);
  ESP_LOGCONFIG(TAG, "  Control handle: 0x%04X", this->control_handle_);
  ESP_LOGCONFIG(TAG, "  Notify handle: 0x%04X", this->notify_handle_);
  if (!this->firmware_version_.empty()) {
    ESP_LOGCONFIG(TAG, "  Firmware version: %s", this->firmware_version_.c_str());
  }
  if (!this->lamp_name_.empty()) {
    ESP_LOGCONFIG(TAG, "  Lamp name: %s", this->lamp_name_.c_str());
  }
}

}  // namespace yeelight_bt
}  // namespace esphome

#endif  // USE_ESP32
