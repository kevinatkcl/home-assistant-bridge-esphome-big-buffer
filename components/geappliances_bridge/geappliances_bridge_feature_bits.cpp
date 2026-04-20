/*!
 * @file
 * @brief Appliance API feature-bit reading and parsing.
 *
 * Reads ERDs 0x0092–0x0097 and 0x0109–0x010D from the appliance after
 * autodiscovery completes.  These ERDs report which appliance-API features
 * are supported and are used to build the filtered ERD list for polling mode
 * and to gate HA discovery to only supported entities.
 *
 * Startup phase order:
 *   run_autodiscovery_() → start_feature_bit_reading_()     (called by device_id.cpp)
 *                        → run_feature_bit_reading_()   ← (this file)
 *                        → parse_and_log_feature_bits_() ← (this file)
 *                        → bridge_init_state_ = WAITING_FOR_MQTT
 */

#include "geappliances_bridge.h"
#include "geappliances_bridge_constants.h"
#include "appliance_api_feature_lists.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace geappliances_bridge {

static const char* const TAG = "geappliances_bridge";

// ---------------------------------------------------------------------------
// Startup: kick off feature-bit reading sequence
// ---------------------------------------------------------------------------

void GeappliancesBridge::start_feature_bit_reading_()
{
  if (this->feature_bit_state_ != FEATURE_BIT_STATE_IDLE) {
    return;
  }
  ESP_LOGI(TAG, "Reading appliance API feature bits (ERDs 0x0092-0x0097 and 0x0109-0x010D)...");
  this->read_retry_count_ = 0;
  this->feature_bit_state_ = FEATURE_BIT_STATE_READING_0092;
}

// ---------------------------------------------------------------------------
// run_feature_bit_reading_() — called every loop() iteration
//
// While any READING_xxxx state is active, this queues the next ERD read and
// transitions to IN_FLIGHT so the next loop() call does not duplicate the
// request.  When the last ERD response is processed (in the GEA callback)
// feature_bit_parse_pending_ is set and the deferred parse runs here.
// ---------------------------------------------------------------------------

void GeappliancesBridge::run_feature_bit_reading_()
{
  if (this->active_erd_client_ == nullptr) {
    return;
  }

  // Deferred parse: called here (not in the GEA callback) so the callback
  // returns quickly and the GEA2 tight-loop continues draining UART bytes
  // without being stalled by the potentially slow parse step.
  if (this->feature_bit_state_ == FEATURE_BIT_STATE_COMPLETE && this->feature_bit_parse_pending_) {
    this->feature_bit_parse_pending_ = false;
    this->parse_and_log_feature_bits_();
    this->bridge_init_state_ = BRIDGE_INIT_STATE_WAITING_FOR_MQTT;
    return;
  }

  // Map current READING state to the ERD we need to read next.
  tiny_erd_t  feature_erd  = 0;
  const char* feature_name = nullptr;

  switch (this->feature_bit_state_) {
    case FEATURE_BIT_STATE_READING_0092: feature_erd = ERD_COMMON_FEATURE_API;      feature_name = "common feature API (0x0092)";          break;
    case FEATURE_BIT_STATE_READING_0093: feature_erd = ERD_APPLIANCE_FEATURE_API_0; feature_name = "appliance feature API 0 (0x0093)";       break;
    case FEATURE_BIT_STATE_READING_0094: feature_erd = ERD_APPLIANCE_FEATURE_API_1; feature_name = "appliance feature API 1 (0x0094)";       break;
    case FEATURE_BIT_STATE_READING_0095: feature_erd = ERD_APPLIANCE_FEATURE_API_2; feature_name = "appliance feature API 2 (0x0095)";       break;
    case FEATURE_BIT_STATE_READING_0096: feature_erd = ERD_APPLIANCE_FEATURE_API_3; feature_name = "appliance feature API 3 (0x0096)";       break;
    case FEATURE_BIT_STATE_READING_0097: feature_erd = ERD_APPLIANCE_FEATURE_API_4; feature_name = "appliance feature API 4 (0x0097)";       break;
    case FEATURE_BIT_STATE_READING_0109: feature_erd = ERD_APPLIANCE_FEATURE_API_5; feature_name = "appliance feature API 5 (0x0109)";       break;
    case FEATURE_BIT_STATE_READING_010A: feature_erd = ERD_APPLIANCE_FEATURE_API_6; feature_name = "appliance feature API 6 (0x010A)";       break;
    case FEATURE_BIT_STATE_READING_010B: feature_erd = ERD_APPLIANCE_FEATURE_API_7; feature_name = "appliance feature API 7 (0x010B)";       break;
    case FEATURE_BIT_STATE_READING_010C: feature_erd = ERD_APPLIANCE_FEATURE_API_8; feature_name = "appliance feature API 8 (0x010C)";       break;
    case FEATURE_BIT_STATE_READING_010D: feature_erd = ERD_APPLIANCE_FEATURE_API_9; feature_name = "appliance feature API 9 (0x010D)";       break;
    default: return;  // IDLE, IN_FLIGHT, COMPLETE, or FAILED — nothing to queue
  }

  if (tiny_gea3_erd_client_read(this->active_erd_client_, &this->pending_request_id_,
                                  this->host_address_, feature_erd)) {
    ESP_LOGD(TAG, "Queued read for %s", feature_name);
    this->read_retry_count_  = 0;
    this->feature_bit_state_ = FEATURE_BIT_STATE_IN_FLIGHT;
  } else {
    this->read_retry_count_++;
    if (this->read_retry_count_ >= MAX_READ_RETRIES) {
      ESP_LOGW(TAG, "Could not queue read for %s after %u attempts, skipping ERD",
               feature_name, MAX_READ_RETRIES);
      this->read_retry_count_ = 0;
      this->skip_to_next_feature_erd_(feature_erd);
    } else if (this->read_retry_count_ % LOG_EVERY_N_RETRIES == 0) {
      ESP_LOGW(TAG, "Failed to queue %s read, retrying... (attempt %u)",
               feature_name, this->read_retry_count_);
    }
  }
}

// ---------------------------------------------------------------------------
// GEA callback handlers (invoked from handle_erd_client_activity_())
// ---------------------------------------------------------------------------

void GeappliancesBridge::process_feature_bit_erd_response_(
  tiny_erd_t erd, const uint8_t* data, uint8_t size)
{
  uint8_t copy_size        = (size <= 8u) ? size : 8u;
  this->read_retry_count_  = 0;

  // Try to immediately queue the next ERD read while still in the GEA callback
  // chain; if the queue is busy, fall back to the corresponding READING state
  // so loop() retries on the next iteration.
  auto queue_next = [this](tiny_erd_t next_erd, FeatureBitState fallback_state) {
    if (this->active_erd_client_ != nullptr &&
        tiny_gea3_erd_client_read(this->active_erd_client_, &this->pending_request_id_,
                                   this->host_address_, next_erd)) {
      this->feature_bit_state_ = FEATURE_BIT_STATE_IN_FLIGHT;
    } else {
      this->feature_bit_state_ = fallback_state;
    }
  };

  if      (erd == ERD_COMMON_FEATURE_API)      { memcpy(this->feature_bit_erd_0092_, data, copy_size); this->feature_bit_erd_0092_size_ = copy_size; ESP_LOGD(TAG, "Read common feature API (0x0092): %u bytes", copy_size);          queue_next(ERD_APPLIANCE_FEATURE_API_0, FEATURE_BIT_STATE_READING_0093); }
  else if (erd == ERD_APPLIANCE_FEATURE_API_0) { memcpy(this->feature_bit_erd_0093_, data, copy_size); this->feature_bit_erd_0093_size_ = copy_size; ESP_LOGD(TAG, "Read appliance feature API 0 (0x0093): %u bytes", copy_size);    queue_next(ERD_APPLIANCE_FEATURE_API_1, FEATURE_BIT_STATE_READING_0094); }
  else if (erd == ERD_APPLIANCE_FEATURE_API_1) { memcpy(this->feature_bit_erd_0094_, data, copy_size); this->feature_bit_erd_0094_size_ = copy_size; ESP_LOGD(TAG, "Read appliance feature API 1 (0x0094): %u bytes", copy_size);    queue_next(ERD_APPLIANCE_FEATURE_API_2, FEATURE_BIT_STATE_READING_0095); }
  else if (erd == ERD_APPLIANCE_FEATURE_API_2) { memcpy(this->feature_bit_erd_0095_, data, copy_size); this->feature_bit_erd_0095_size_ = copy_size; ESP_LOGD(TAG, "Read appliance feature API 2 (0x0095): %u bytes", copy_size);    queue_next(ERD_APPLIANCE_FEATURE_API_3, FEATURE_BIT_STATE_READING_0096); }
  else if (erd == ERD_APPLIANCE_FEATURE_API_3) { memcpy(this->feature_bit_erd_0096_, data, copy_size); this->feature_bit_erd_0096_size_ = copy_size; ESP_LOGD(TAG, "Read appliance feature API 3 (0x0096): %u bytes", copy_size);    queue_next(ERD_APPLIANCE_FEATURE_API_4, FEATURE_BIT_STATE_READING_0097); }
  else if (erd == ERD_APPLIANCE_FEATURE_API_4) { memcpy(this->feature_bit_erd_0097_, data, copy_size); this->feature_bit_erd_0097_size_ = copy_size; ESP_LOGD(TAG, "Read appliance feature API 4 (0x0097): %u bytes", copy_size);    queue_next(ERD_APPLIANCE_FEATURE_API_5, FEATURE_BIT_STATE_READING_0109); }
  else if (erd == ERD_APPLIANCE_FEATURE_API_5) { memcpy(this->feature_bit_erd_0109_, data, copy_size); this->feature_bit_erd_0109_size_ = copy_size; ESP_LOGD(TAG, "Read appliance feature API 5 (0x0109): %u bytes", copy_size);    queue_next(ERD_APPLIANCE_FEATURE_API_6, FEATURE_BIT_STATE_READING_010A); }
  else if (erd == ERD_APPLIANCE_FEATURE_API_6) { memcpy(this->feature_bit_erd_010A_, data, copy_size); this->feature_bit_erd_010A_size_ = copy_size; ESP_LOGD(TAG, "Read appliance feature API 6 (0x010A): %u bytes", copy_size);    queue_next(ERD_APPLIANCE_FEATURE_API_7, FEATURE_BIT_STATE_READING_010B); }
  else if (erd == ERD_APPLIANCE_FEATURE_API_7) { memcpy(this->feature_bit_erd_010B_, data, copy_size); this->feature_bit_erd_010B_size_ = copy_size; ESP_LOGD(TAG, "Read appliance feature API 7 (0x010B): %u bytes", copy_size);    queue_next(ERD_APPLIANCE_FEATURE_API_8, FEATURE_BIT_STATE_READING_010C); }
  else if (erd == ERD_APPLIANCE_FEATURE_API_8) { memcpy(this->feature_bit_erd_010C_, data, copy_size); this->feature_bit_erd_010C_size_ = copy_size; ESP_LOGD(TAG, "Read appliance feature API 8 (0x010C): %u bytes", copy_size);    queue_next(ERD_APPLIANCE_FEATURE_API_9, FEATURE_BIT_STATE_READING_010D); }
  else if (erd == ERD_APPLIANCE_FEATURE_API_9) {
    memcpy(this->feature_bit_erd_010D_, data, copy_size);
    this->feature_bit_erd_010D_size_ = copy_size;
    ESP_LOGD(TAG, "Read appliance feature API 9 (0x010D): %u bytes", copy_size);
    // Last ERD in the chain: defer parsing to loop() so this callback returns
    // quickly and the GEA2 tight-loop continues processing UART bytes.
    this->feature_bit_state_       = FEATURE_BIT_STATE_COMPLETE;
    this->feature_bit_parse_pending_ = true;
  }
}

void GeappliancesBridge::handle_feature_bit_read_failure_(tiny_erd_t erd)
{
  // Not every appliance implements every slot; a failure here is normal.
  ESP_LOGD(TAG, "Feature bit ERD 0x%04X failed or not supported, skipping", erd);
  this->skip_to_next_feature_erd_(erd);
}

void GeappliancesBridge::skip_to_next_feature_erd_(tiny_erd_t failed_erd)
{
  // Advance the state machine to the READING state for the next ERD in the
  // chain.  For the last ERD (0x010D) trigger the same deferred-parse path
  // used on a successful final read.
  switch (failed_erd) {
    case ERD_COMMON_FEATURE_API:      this->feature_bit_state_ = FEATURE_BIT_STATE_READING_0093; break;
    case ERD_APPLIANCE_FEATURE_API_0: this->feature_bit_state_ = FEATURE_BIT_STATE_READING_0094; break;
    case ERD_APPLIANCE_FEATURE_API_1: this->feature_bit_state_ = FEATURE_BIT_STATE_READING_0095; break;
    case ERD_APPLIANCE_FEATURE_API_2: this->feature_bit_state_ = FEATURE_BIT_STATE_READING_0096; break;
    case ERD_APPLIANCE_FEATURE_API_3: this->feature_bit_state_ = FEATURE_BIT_STATE_READING_0097; break;
    case ERD_APPLIANCE_FEATURE_API_4: this->feature_bit_state_ = FEATURE_BIT_STATE_READING_0109; break;
    case ERD_APPLIANCE_FEATURE_API_5: this->feature_bit_state_ = FEATURE_BIT_STATE_READING_010A; break;
    case ERD_APPLIANCE_FEATURE_API_6: this->feature_bit_state_ = FEATURE_BIT_STATE_READING_010B; break;
    case ERD_APPLIANCE_FEATURE_API_7: this->feature_bit_state_ = FEATURE_BIT_STATE_READING_010C; break;
    case ERD_APPLIANCE_FEATURE_API_8: this->feature_bit_state_ = FEATURE_BIT_STATE_READING_010D; break;
    case ERD_APPLIANCE_FEATURE_API_9:
      this->feature_bit_state_       = FEATURE_BIT_STATE_COMPLETE;
      this->feature_bit_parse_pending_ = true;
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Parse all collected feature-bit ERD data and build the valid-ERD set.
// Called from run_feature_bit_reading_() (deferred from the GEA callback).
// ---------------------------------------------------------------------------

void GeappliancesBridge::parse_and_log_feature_bits_()
{
  this->appliance_api_valid_erds_.clear();
  this->appliance_api_valid_erds_vec_.clear();
  this->appliance_api_valid_list_ready_ = false;

  // Parse common features from ERD 0x0092.
  if (this->feature_bit_erd_0092_size_ > 0) {
    uint32_t common_bits = static_cast<uint32_t>(
      read_be64(this->feature_bit_erd_0092_, this->feature_bit_erd_0092_size_) & 0xFFFFFFFFu);
    ESP_LOGI(TAG, "Common feature API (0x0092) value: 0x%08X", common_bits);
    for (uint16_t i = 0; i < common_feature_descriptor_count; i++) {
      const auto& desc = common_feature_descriptors[i];
      if (common_bits & desc.bit_mask) {
        ESP_LOGI(TAG, "  [SET] Common feature: %s (mask 0x%08X, %u ERDs)",
                 desc.name, desc.bit_mask, desc.erd_count);
        for (uint16_t j = 0; j < desc.erd_count; j++) {
          this->appliance_api_valid_erds_.insert(desc.erds[j]);
        }
      }
    }
  }

  // Parse appliance feature APIs from ERDs 0x0093–0x0097 and 0x0109–0x010D.
  // Layout: [2B featureType][2B version][4B feature bitmap]
  const uint8_t* api_bufs[10] = {
    this->feature_bit_erd_0093_, this->feature_bit_erd_0094_,
    this->feature_bit_erd_0095_, this->feature_bit_erd_0096_,
    this->feature_bit_erd_0097_, this->feature_bit_erd_0109_,
    this->feature_bit_erd_010A_, this->feature_bit_erd_010B_,
    this->feature_bit_erd_010C_, this->feature_bit_erd_010D_
  };
  const uint8_t api_sizes[10] = {
    this->feature_bit_erd_0093_size_, this->feature_bit_erd_0094_size_,
    this->feature_bit_erd_0095_size_, this->feature_bit_erd_0096_size_,
    this->feature_bit_erd_0097_size_, this->feature_bit_erd_0109_size_,
    this->feature_bit_erd_010A_size_, this->feature_bit_erd_010B_size_,
    this->feature_bit_erd_010C_size_, this->feature_bit_erd_010D_size_
  };
  static const char* const erd_names[10] = {
    "0x0093", "0x0094", "0x0095", "0x0096", "0x0097",
    "0x0109", "0x010A", "0x010B", "0x010C", "0x010D"
  };

  for (uint8_t idx = 0; idx < 10; idx++) {
    if (api_sizes[idx] < APPLIANCE_FEATURE_ERD_SIZE) continue;
    const uint8_t* buf         = api_bufs[idx];
    uint16_t appliance_type    = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
    uint16_t version           = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
    uint32_t feature_bitmap    = (static_cast<uint32_t>(buf[4]) << 24) |
                                 (static_cast<uint32_t>(buf[5]) << 16) |
                                 (static_cast<uint32_t>(buf[6]) <<  8) | buf[7];
    if (appliance_type == 0 && version == 0 && feature_bitmap == 0) continue;
    ESP_LOGI(TAG, "Appliance feature ERD %s: type 0x%04X, version %u, features 0x%08X",
             erd_names[idx], appliance_type, version, feature_bitmap);
    bool found_descriptor = false;
    for (uint16_t i = 0; i < appliance_feature_api_descriptor_count; i++) {
      const auto& desc = appliance_feature_api_descriptors[i];
      if (desc.feature_type != appliance_type || desc.version != version) continue;
      found_descriptor = true;
      if (feature_bitmap & desc.bit_mask) {
        ESP_LOGI(TAG, "  [SET] %s (mask 0x%08X, %u ERDs)",
                 desc.name, desc.bit_mask, desc.erd_count);
        for (uint16_t j = 0; j < desc.erd_count; j++) {
          this->appliance_api_valid_erds_.insert(desc.erds[j]);
        }
      }
    }
    if (!found_descriptor) {
      ESP_LOGW(TAG, "  No known API definition for type 0x%04X version %u",
               appliance_type, version);
    }
  }

  // Mandatory ERDs are always published regardless of feature bit results.
  this->appliance_api_valid_erds_.insert(ERD_MODEL_NUMBER);
  this->appliance_api_valid_erds_.insert(ERD_SERIAL_NUMBER);
  this->appliance_api_valid_erds_.insert(ERD_APPLIANCE_TYPE);
  this->appliance_api_valid_erds_.insert(ERD_COMMON_FEATURE_API);
  this->appliance_api_valid_erds_.insert(ERD_APPLIANCE_FEATURE_API_0);
  this->appliance_api_valid_erds_.insert(ERD_APPLIANCE_FEATURE_API_1);
  this->appliance_api_valid_erds_.insert(ERD_APPLIANCE_FEATURE_API_2);
  this->appliance_api_valid_erds_.insert(ERD_APPLIANCE_FEATURE_API_3);
  this->appliance_api_valid_erds_.insert(ERD_APPLIANCE_FEATURE_API_4);
  this->appliance_api_valid_erds_.insert(ERD_APPLIANCE_FEATURE_API_5);
  this->appliance_api_valid_erds_.insert(ERD_APPLIANCE_FEATURE_API_6);
  this->appliance_api_valid_erds_.insert(ERD_APPLIANCE_FEATURE_API_7);
  this->appliance_api_valid_erds_.insert(ERD_APPLIANCE_FEATURE_API_8);
  this->appliance_api_valid_erds_.insert(ERD_APPLIANCE_FEATURE_API_9);

  // Build a sorted vector for passing to the polling bridge as a C array.
  this->appliance_api_valid_erds_vec_.assign(
    this->appliance_api_valid_erds_.begin(),
    this->appliance_api_valid_erds_.end());

  ESP_LOGI(TAG, "Appliance API feature parsing complete: %zu valid ERDs identified",
           this->appliance_api_valid_erds_vec_.size());
  this->appliance_api_valid_list_ready_ = true;
}

}  // namespace geappliances_bridge
}  // namespace esphome
