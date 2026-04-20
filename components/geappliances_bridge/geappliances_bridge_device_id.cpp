/*!
 * @file
 * @brief Device ID generation from appliance identity ERDs.
 *
 * Reads ERDs 0x0008 (appliance type), 0x0001 (model number), and 0x0002
 * (serial number) after feature-bit reading completes and assembles a unique,
 * MQTT-topic-safe device identifier string.  If a device_id is already
 * configured in YAML, the read sequence is skipped and that value is used
 * directly.
 *
 * Startup phase order:
 *   run_autodiscovery_() → start_device_id_generation_()
 *                        → run_device_id_generation_()   ← (this file)
 *                        → start_feature_bit_reading_()
 */

#include "geappliances_bridge.h"
#include "geappliances_bridge_constants.h"
#include "esphome/core/log.h"
#include <cstring>
#include <inttypes.h>

// Forward declaration (generated from appliance API data)
std::string appliance_type_to_string(uint8_t appliance_type);

namespace esphome {
namespace geappliances_bridge {

static const char* const TAG = "geappliances_bridge";

// ---------------------------------------------------------------------------
// Startup: begin device ID generation (or skip if already configured)
// ---------------------------------------------------------------------------

void GeappliancesBridge::start_device_id_generation_()
{
  if (this->device_id_state_ != DEVICE_ID_STATE_IDLE) {
    return;
  }

  if (!this->configured_device_id_.empty()) {
    ESP_LOGI(TAG, "Using configured device_id: %s", this->configured_device_id_.c_str());
    this->final_device_id_  = this->configured_device_id_;
    this->device_id_state_  = DEVICE_ID_STATE_COMPLETE;
    this->start_feature_bit_reading_();
    return;
  }

  const char* protocol = this->gea2_protocol_active_ ? "GEA2" : "GEA3";
  ESP_LOGI(TAG, "Starting device ID generation from host address 0x%02X via %s",
           this->host_address_, protocol);
  this->device_id_state_ = DEVICE_ID_STATE_READING_APPLIANCE_TYPE;
}

// ---------------------------------------------------------------------------
// run_device_id_generation_() — called every loop() iteration
//
// While any READING_* state is active, attempts to queue the corresponding
// ERD read.  Sets state to IDLE while waiting for the response.
// ---------------------------------------------------------------------------

void GeappliancesBridge::run_device_id_generation_()
{
  if (this->active_erd_client_ == nullptr) {
    return;
  }

  if (this->device_id_state_ == DEVICE_ID_STATE_READING_APPLIANCE_TYPE) {
    this->try_read_erd_with_retry_(ERD_APPLIANCE_TYPE, "appliance type");
  } else if (this->device_id_state_ == DEVICE_ID_STATE_READING_MODEL_NUMBER) {
    this->try_read_erd_with_retry_(ERD_MODEL_NUMBER, "model number");
  } else if (this->device_id_state_ == DEVICE_ID_STATE_READING_SERIAL_NUMBER) {
    this->try_read_erd_with_retry_(ERD_SERIAL_NUMBER, "serial number");
  }
}

// ---------------------------------------------------------------------------
// GEA callback handlers (invoked from handle_erd_client_activity_())
// ---------------------------------------------------------------------------

void GeappliancesBridge::process_device_id_erd_response_(
  tiny_erd_t erd, const uint8_t* data, uint8_t size)
{
  this->device_id_response_retries_ = 0;

  if (erd == ERD_APPLIANCE_TYPE) {
    if (size < 1) return;
    this->appliance_type_ = data[0];
    ESP_LOGI(TAG, "Read appliance type: %u", this->appliance_type_);
    // Queue the model-number read immediately, while still inside the GEA2
    // tight-loop callback chain. This mirrors what the polling bridge does in
    // state_add_common_erds and ensures the request is sent within the same
    // 200 ms tight-loop window so the large (32-byte) response can be received
    // before the window expires. Deferring to the next loop() call crosses the
    // ~50 ms ESPHome overhead gap which can cause timing issues for large ERDs.
    if (this->active_erd_client_ != nullptr &&
        tiny_gea3_erd_client_read(this->active_erd_client_, &this->pending_request_id_,
                                   this->host_address_, ERD_MODEL_NUMBER)) {
      this->device_id_state_ = DEVICE_ID_STATE_IDLE;  // waiting for model-number response
    } else {
      this->device_id_state_ = DEVICE_ID_STATE_READING_MODEL_NUMBER;
    }

  } else if (erd == ERD_MODEL_NUMBER) {
    this->model_number_ = this->bytes_to_string_(data, size);
    ESP_LOGI(TAG, "Read model number: %s", this->model_number_.c_str());
    // Queue the serial-number read immediately (same reasoning as above).
    if (this->active_erd_client_ != nullptr &&
        tiny_gea3_erd_client_read(this->active_erd_client_, &this->pending_request_id_,
                                   this->host_address_, ERD_SERIAL_NUMBER)) {
      this->device_id_state_ = DEVICE_ID_STATE_IDLE;  // waiting for serial-number response
    } else {
      this->device_id_state_ = DEVICE_ID_STATE_READING_SERIAL_NUMBER;
    }

  } else if (erd == ERD_SERIAL_NUMBER) {
    this->serial_number_ = this->bytes_to_string_(data, size);
    ESP_LOGI(TAG, "Read serial number: %s", this->serial_number_.c_str());

    std::string appliance_type_name = appliance_type_to_string(this->appliance_type_);
    this->generated_device_id_ = appliance_type_name + "_" +
                                 this->sanitize_for_mqtt_topic_(this->model_number_) + "_" +
                                 this->sanitize_for_mqtt_topic_(this->serial_number_);
    this->final_device_id_  = this->generated_device_id_;
    ESP_LOGI(TAG, "Generated device ID: %s", this->final_device_id_.c_str());

    this->device_id_state_  = DEVICE_ID_STATE_COMPLETE;
    this->start_feature_bit_reading_();
  }
}

void GeappliancesBridge::handle_device_id_read_failure_(tiny_erd_t erd)
{
  this->device_id_response_retries_++;
  if (this->device_id_response_retries_ < MAX_DEVICE_ID_RESPONSE_RETRIES) {
    // Retry the same ERD.
    if      (erd == ERD_APPLIANCE_TYPE) this->device_id_state_ = DEVICE_ID_STATE_READING_APPLIANCE_TYPE;
    else if (erd == ERD_MODEL_NUMBER)   this->device_id_state_ = DEVICE_ID_STATE_READING_MODEL_NUMBER;
    else if (erd == ERD_SERIAL_NUMBER)  this->device_id_state_ = DEVICE_ID_STATE_READING_SERIAL_NUMBER;
    return;
  }

  // Too many failures — use a fallback value and advance so device ID
  // generation can always complete even on appliances missing these ERDs.
  this->device_id_response_retries_ = 0;
  ESP_LOGW(TAG, "ERD 0x%04X unreadable after %u attempts, using fallback for device ID",
           erd, MAX_DEVICE_ID_RESPONSE_RETRIES);

  if (erd == ERD_APPLIANCE_TYPE) {
    this->appliance_type_  = 0;  // Unknown
    this->device_id_state_ = DEVICE_ID_STATE_READING_MODEL_NUMBER;

  } else if (erd == ERD_MODEL_NUMBER) {
    this->model_number_    = "";  // empty — omitted from device ID
    this->device_id_state_ = DEVICE_ID_STATE_READING_SERIAL_NUMBER;

  } else if (erd == ERD_SERIAL_NUMBER) {
    // Use the bus address as a serial-number substitute so the device ID
    // remains unique per appliance on the local bus.
    char addr_str[8];
    snprintf(addr_str, sizeof(addr_str), "%02X", this->host_address_);
    this->serial_number_ = std::string("busaddr") + addr_str;

    std::string appliance_type_name = appliance_type_to_string(this->appliance_type_);
    this->generated_device_id_ = appliance_type_name + "_" +
                                 this->sanitize_for_mqtt_topic_(this->model_number_) + "_" +
                                 this->sanitize_for_mqtt_topic_(this->serial_number_);
    this->final_device_id_  = this->generated_device_id_;
    ESP_LOGI(TAG, "Generated device ID (with fallback): %s", this->final_device_id_.c_str());

    this->device_id_state_  = DEVICE_ID_STATE_COMPLETE;
    this->start_feature_bit_reading_();
  }
}

// ---------------------------------------------------------------------------
// ERD queue helper
// ---------------------------------------------------------------------------

bool GeappliancesBridge::try_read_erd_with_retry_(tiny_erd_t erd, const char* erd_name)
{
  if (tiny_gea3_erd_client_read(this->active_erd_client_, &this->pending_request_id_,
                                 this->host_address_, erd)) {
    ESP_LOGD(TAG, "Reading %s ERD 0x%04X", erd_name, erd);
    this->device_id_state_  = DEVICE_ID_STATE_IDLE;  // wait for response
    this->read_retry_count_ = 0;
    return true;
  }

  this->read_retry_count_++;
  if (this->read_retry_count_ >= MAX_READ_RETRIES) {
    ESP_LOGE(TAG, "Failed to read %s after %u retries, giving up", erd_name, MAX_READ_RETRIES);
    this->device_id_state_ = DEVICE_ID_STATE_FAILED;
    return false;
  }
  if (this->read_retry_count_ % LOG_EVERY_N_RETRIES == 0) {
    ESP_LOGW(TAG, "Failed to queue %s read, retrying... (attempt %u)",
             erd_name, this->read_retry_count_);
  }
  return false;
}

// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------

std::string GeappliancesBridge::bytes_to_string_(const uint8_t* data, size_t size)
{
  if (data == nullptr || size == 0) {
    return "";
  }
  std::string result;
  result.reserve(size);
  for (size_t i = 0; i < size; i++) {
    if (data[i] == 0x00) break;  // stop at null terminator
    result += static_cast<char>(data[i]);
  }
  return result;
}

std::string GeappliancesBridge::sanitize_for_mqtt_topic_(const std::string& input)
{
  std::string result;
  result.reserve(input.length());
  for (char c : input) {
    if (c == '+' || c == '#' || c == '\0' || c == ' ' || c == '/' || c == '$' ||
        c < 32 || c > 126) {
      result += '_';
    } else {
      result += c;
    }
  }
  return result;
}

}  // namespace geappliances_bridge
}  // namespace esphome
