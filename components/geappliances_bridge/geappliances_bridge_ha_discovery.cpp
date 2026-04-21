/*!
 * @file
 * @brief Home Assistant device-discovery publishing.
 *
 * After the MQTT bridge is initialized and ERD registration has settled,
 * run_ha_discovery_() triggers an HTTPS fetch of per-category JSONL definition
 * files.  The fetch runs in a FreeRTOS background task (ESP-IDF only); each
 * parsed entity is pushed onto a queue and published by the main loop at a
 * rate-limited pace to avoid flooding the IDF MQTT event queue.
 *
 * On non-IDF builds the feature is unavailable and a clear warning is logged.
 */

#include "geappliances_bridge.h"
#include "esphome/core/log.h"
#include <cstring>

#ifdef USE_ESP_IDF
#  include "esp_http_client.h"
#  include "esp_crt_bundle.h"
#  include "cJSON.h"
#  include "freertos/FreeRTOS.h"
#  include "freertos/task.h"
#  include "freertos/queue.h"
#endif  // USE_ESP_IDF

namespace esphome {
namespace geappliances_bridge {

static const char* const TAG = "geappliances_bridge";

// ---------------------------------------------------------------------------
// Static helper: JSON string escaping
// ---------------------------------------------------------------------------

static std::string escape_json_str(const std::string& s)
{
  std::string out;
  out.reserve(s.size() + 4);
  for (unsigned char c : s) {
    if      (c == '"')  { out += "\\\""; }
    else if (c == '\\') { out += "\\\\"; }
    else if (c < 0x20)  { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c)); out += buf; }
    else                { out += static_cast<char>(c); }
  }
  return out;
}

// ---------------------------------------------------------------------------
// run_ha_discovery_() — called every loop() iteration
//
// Handles two sub-tasks:
//   1. Watches for the "ready" signal (quiet window or polling_list_complete)
//      and kicks off the HTTPS fetch task.
//   2. Drains the fetch queue one entity per rate-limited interval.
// ---------------------------------------------------------------------------

void GeappliancesBridge::run_ha_discovery_()
{
  // ── Part 1: check whether it is time to start the fetch ─────────────────
  if (this->ha_discovery_pending_ && !this->ha_discovery_publish_in_progress_) {
    bool is_poll_mode = !((this->mode_ == BRIDGE_MODE_SUBSCRIBE) ||
                          (this->mode_ == BRIDGE_MODE_AUTO && this->subscription_mode_active_));
    bool ready = false;

    if (is_poll_mode) {
      // Wait until the polling HSM has completed ERD discovery (state_polling).
      ready = this->mqtt_bridge_polling_.polling_list_complete;
    } else {
      // Subscription mode: use the 10-second quiet window after the last new ERD.
      //
      // In AUTO mode the bridge starts in subscription mode but may fall back
      // to polling if the appliance does not respond. Only start the countdown
      // once subscription activity has actually been detected; if no activity
      // arrives the bridge will eventually fall back and the polling gate above
      // will fire instead.
      bool subscription_confirmed = (this->mode_ == BRIDGE_MODE_SUBSCRIBE) ||
                                     this->subscription_activity_detected_;
      if (subscription_confirmed) {
        bool subscription_quiet =
          (millis() - this->ha_discovery_last_activity_ >= HA_DISCOVERY_QUIET_MS);
        if (subscription_quiet) {
          // Wait for the custom ERD bridge's first poll cycle to complete
          // before generating HA discovery payloads.
          bool custom_erds_ready = this->custom_erds_vec_.empty() ||
                                   this->custom_erd_bridge_.polling_list_complete;
          ready = custom_erds_ready;
        }
      }
    }

    if (ready) {
      this->publish_ha_discovery_();
    }
  }

  // ── Part 2: drain the queue one entity per rate-limited interval ─────────
  if (this->ha_discovery_publish_in_progress_) {
    uint32_t now = millis();
    if (now - this->ha_entity_last_publish_ms_ >= HA_ENTITY_PUBLISH_INTERVAL_MS) {
      this->ha_entity_last_publish_ms_ = now;
      this->publish_next_ha_discovery_entity_();
    }
  }
}

// ---------------------------------------------------------------------------
// Handle new-ERD events from handle_erd_client_activity_()
//
// Resets the subscription-mode quiet window when a NEW ERD ID is seen.
// Repeated value updates for already-known ERDs do not extend the wait.
// ---------------------------------------------------------------------------

void GeappliancesBridge::on_ha_discovery_erd_seen_(tiny_erd_t erd)
{
  if (!this->ha_discovery_pending_ || this->ha_discovery_published_) {
    return;
  }
  if (this->ha_discovery_seen_erds_.find(erd) == this->ha_discovery_seen_erds_.end()) {
    this->ha_discovery_seen_erds_.insert(erd);
    this->ha_discovery_last_activity_ = millis();
  }
}

// ---------------------------------------------------------------------------
// Kick off the HTTPS fetch (spawns a FreeRTOS task on ESP-IDF)
// ---------------------------------------------------------------------------

void GeappliancesBridge::publish_ha_discovery_()
{
  auto* mqtt_client = esphome::mqtt::global_mqtt_client;
  if (mqtt_client == nullptr || !mqtt_client->is_connected()) {
    ESP_LOGW(TAG, "MQTT not connected, skipping HA discovery publish");
    return;
  }

#ifdef USE_ESP_IDF
  this->ha_discovery_queue_ = xQueueCreate(20, sizeof(HaDiscoveryItem*));
  if (!this->ha_discovery_queue_) {
    ESP_LOGE(TAG, "HA discovery: failed to create queue (OOM)");
    return;
  }

  // Snapshot the registered-ERD set before spawning the background task.
  // The task reads this snapshot exclusively so it never races with concurrent
  // ha_registered_erds_.insert() calls from the main loop (e.g. new
  // subscription ERDs arriving while the HTTPS fetch is in progress).
  this->ha_registered_erds_snapshot_ = this->ha_registered_erds_;

  this->ha_discovery_pending_             = false;
  this->ha_discovery_publish_in_progress_ = true;
  ESP_LOGI(TAG, "HA discovery: starting — %zu ERDs registered, launching fetch task",
           this->ha_registered_erds_snapshot_.size());

  // Stack size 24 KB – HTTPS (mbedTLS handshake alone needs ~10 KB) + cJSON
  // parsing + std::string operations push the peak stack usage well above
  // 12 KB.  A stack overflow corrupts heap metadata and causes the FreeRTOS
  // idle task to fault in prvCheckTasksWaitingTermination when it tries to
  // free the terminated task's TCB/stack (observed crash on ESP32-C6).
  // Priority 1: below IDF MQTT task (5) so MQTT events are not starved while
  // the fetch task is parsing JSONL lines and filling the queue.
  BaseType_t rc = xTaskCreate(ha_fetch_task_fn_, "ha_fetch", 24576, this, 1,
                              &this->ha_fetch_task_handle_);
  if (rc != pdPASS) {
    ESP_LOGE(TAG, "HA discovery: failed to create fetch task (rc=%d)", static_cast<int>(rc));
    vQueueDelete(this->ha_discovery_queue_);
    this->ha_discovery_queue_               = nullptr;
    this->ha_fetch_task_handle_             = nullptr;
    this->ha_discovery_publish_in_progress_ = false;
  }
#else
  ESP_LOGW(TAG, "Home Assistant discovery is only available with the ESP-IDF framework. "
                "Add 'framework:\\n  type: esp-idf' to your esp32: section to enable it.");
  this->ha_discovery_pending_             = false;
  this->ha_discovery_publish_in_progress_ = false;
  this->ha_discovery_published_           = true;
#endif
}

// ---------------------------------------------------------------------------
// Drain one entity from the queue (rate-limited, called from run_ha_discovery_)
// ---------------------------------------------------------------------------

void GeappliancesBridge::publish_next_ha_discovery_entity_()
{
#ifdef USE_ESP_IDF
  if (!this->ha_discovery_queue_) return;

  auto* mqtt_client = esphome::mqtt::global_mqtt_client;
  if (mqtt_client == nullptr || !mqtt_client->is_connected()) {
    return;  // MQTT dropped mid-sequence; resume on reconnect
  }

  HaDiscoveryItem* item = nullptr;
  if (xQueueReceive(this->ha_discovery_queue_, &item, 0) == pdTRUE) {
    if (item == nullptr) {
      // nullptr sentinel: fetch task has finished.
      ESP_LOGI(TAG, "HA discovery: complete — all entities published");
      this->ha_discovery_publish_in_progress_ = false;
      this->ha_discovery_published_           = true;
      vQueueDelete(this->ha_discovery_queue_);
      this->ha_discovery_queue_   = nullptr;
      this->ha_fetch_task_handle_ = nullptr;
    } else {
      mqtt_client->publish(item->topic, item->payload, 0, true);  // QoS 0, retain
      ESP_LOGD(TAG, "HA discovery: published %s", item->topic.c_str());
      delete item;
    }
  }
#endif
}

// ---------------------------------------------------------------------------
// Background FreeRTOS task: HTTP fetch + JSONL parse (ESP-IDF only)
// ---------------------------------------------------------------------------

#ifdef USE_ESP_IDF

/*static*/ void GeappliancesBridge::ha_fetch_task_fn_(void* param)
{
  auto* self = static_cast<GeappliancesBridge*>(param);
  self->fetch_ha_definitions_();

  // Send a nullptr sentinel so the main loop knows the fetch is done.
  HaDiscoveryItem* sentinel = nullptr;
  xQueueSend(self->ha_discovery_queue_, &sentinel, portMAX_DELAY);

  vTaskDelete(nullptr);
}

void GeappliancesBridge::fetch_ha_definitions_()
{
  // Determine which category files are needed based on registered ERD ranges.
  struct Category { const char* name; uint16_t lo; uint16_t hi; };
  static const Category CATS[] = {
    {"common",          0x0000, 0x0FFF},
    {"refrigeration",   0x1000, 0x1FFF},
    {"laundry",         0x2000, 0x2FFF},
    {"dishwasher",      0x3000, 0x3FFF},
    {"waterheater",     0x4000, 0x4FFF},
    {"range",           0x5000, 0x5FFF},
    {"airconditioning", 0x7000, 0x7FFF},
    {"waterfilter",     0x8000, 0x8FFF},
    {"smallappliance",  0x9000, 0x9FFF},
    {"energy",          0xD000, 0xDFFF},
  };

  bool need[10] = {};
  need[0] = true;  // common — always needed
  for (uint16_t erd : this->ha_registered_erds_snapshot_) {
    for (int i = 1; i < 10; ++i) {
      if (erd >= CATS[i].lo && erd <= CATS[i].hi) { need[i] = true; break; }
    }
  }

  // Build device JSON once (reused for every entity in this fetch run).
  const std::string& device_id = this->final_device_id_;
  std::string device_json = "{\"identifiers\":[\"" + device_id + "\"]";
  device_json += ",\"name\":\"" + escape_json_str(device_id) + "\"";
  device_json += ",\"manufacturer\":\"GE Appliances\"";
  if (!this->model_number_.empty())
    device_json += ",\"model\":\"" + escape_json_str(this->model_number_) + "\"";
  if (!this->serial_number_.empty())
    device_json += ",\"serial_number\":\"" + escape_json_str(this->serial_number_) + "\"";
  device_json += "}";

  for (int i = 0; i < 10; ++i) {
    if (!need[i]) continue;
    std::string url = this->ha_discovery_base_url_ + "/" + CATS[i].name + ".jsonl";
    ESP_LOGI(TAG, "HA fetch: downloading %s", url.c_str());
    fetch_category_(url, device_id, device_json);
    vTaskDelay(pdMS_TO_TICKS(50));  // brief yield between requests
  }
}

bool GeappliancesBridge::fetch_category_(const std::string& url,
                                          const std::string& device_id,
                                          const std::string& device_json)
{
  esp_http_client_config_t cfg = {};
  cfg.url                     = url.c_str();
  cfg.crt_bundle_attach       = esp_crt_bundle_attach;
  cfg.timeout_ms              = 20000;
  cfg.max_redirection_count   = 5;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    ESP_LOGE(TAG, "HA fetch: esp_http_client_init failed for %s", url.c_str());
    return false;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HA fetch: open failed for %s: %s", url.c_str(), esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }

  esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (status == 404) {
    ESP_LOGW(TAG, "HA fetch: %s not found (404) — check ha_discovery_base_url", url.c_str());
    esp_http_client_cleanup(client);
    return true;
  }
  if (status != 200) {
    ESP_LOGW(TAG, "HA fetch: HTTP %d for %s", status, url.c_str());
    esp_http_client_cleanup(client);
    return false;
  }

  // Read response body line by line. Each JSONL line is one entity.
  // LINE_BUF must accommodate the largest lines (select entities with many
  // enum values can reach ~7200 bytes).
  static constexpr int READ_BUF = 512;
  static constexpr int LINE_BUF = 8192;
  char  read_buf[READ_BUF];
  char* line_buf = static_cast<char*>(malloc(LINE_BUF));
  if (!line_buf) {
    ESP_LOGE(TAG, "HA fetch: OOM allocating line buffer");
    esp_http_client_cleanup(client);
    return false;
  }

  int line_pos = 0;
  int entities = 0;
  int read_len;
  while ((read_len = esp_http_client_read(client, read_buf, READ_BUF - 1)) > 0) {
    for (int i = 0; i < read_len; ++i) {
      char c = read_buf[i];
      if (c == '\n' || c == '\r') {
        if (line_pos > 2) {
          line_buf[line_pos] = '\0';
          if (process_jsonl_line_(line_buf, device_id, device_json)) ++entities;
        }
        line_pos = 0;
      } else if (line_pos < LINE_BUF - 1) {
        line_buf[line_pos++] = c;
      }
      // Lines exceeding LINE_BUF are silently truncated and will not parse.
    }
  }
  if (line_pos > 2) {
    line_buf[line_pos] = '\0';
    if (process_jsonl_line_(line_buf, device_id, device_json)) ++entities;
  }

  free(line_buf);
  esp_http_client_cleanup(client);
  ESP_LOGI(TAG, "HA fetch: %s → %d entities queued", url.c_str(), entities);
  return true;
}

bool GeappliancesBridge::process_jsonl_line_(const char* line,
                                              const std::string& device_id,
                                              const std::string& device_json)
{
  cJSON* root = cJSON_Parse(line);
  if (!root) return false;

  auto get_str = [&](const char* key) -> const char* {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return (item && cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
  };

  const char* erd_hex = get_str("i");
  if (erd_hex[0] == '\0') { cJSON_Delete(root); return false; }

  uint16_t    erd_id = static_cast<uint16_t>(strtol(erd_hex, nullptr, 16));
  const char* domain = get_str("d");
  const char* name   = get_str("n");
  const char* role   = get_str("r");
  const char* paired = get_str("p");

  // Filter: only publish entities for ERDs the device actually registered.
  if (!this->ha_registered_erds_snapshot_.empty()) {
    bool registered = this->ha_registered_erds_snapshot_.count(erd_id) > 0;
    if (!registered && role[0] == 'r' && paired[0] != '\0') {
      uint16_t paired_id = static_cast<uint16_t>(strtol(paired, nullptr, 16));
      if (paired_id)
        registered = this->ha_registered_erds_snapshot_.count(paired_id) > 0 ||
                     this->ha_registered_erds_snapshot_.count(erd_id) > 0;
    }
    if (!registered) { cJSON_Delete(root); return false; }
  }

  char erd_id_str[5];
  snprintf(erd_id_str, sizeof(erd_id_str), "%04x", erd_id);

  bool        is_request    = (role[0] == 'r');
  std::string state_topic, command_topic;
  if (is_request && paired[0] != '\0') {
    state_topic   = "geappliances/" + device_id + "/erd/0x" + std::string(paired) + "/value";
    command_topic = "geappliances/" + device_id + "/erd/0x" + erd_id_str + "/write";
  } else {
    state_topic   = "geappliances/" + device_id + "/erd/0x" + erd_id_str + "/value";
    command_topic = "geappliances/" + device_id + "/erd/0x" + erd_id_str + "/write";
  }

  const char* field_id  = get_str("fi");
  std::string unique_id = device_id + "_" + erd_id_str;
  if (field_id[0] != '\0') { unique_id += "_"; unique_id += field_id; }

  const char* vt   = get_str("vt");
  const char* ct   = get_str("ct");
  const char* opts = get_str("o");
  const char* unit = get_str("u");
  const char* dc   = get_str("dc");
  const char* sc   = get_str("sc");

  std::string payload;
  auto add_field = [&](const char* key, const char* val) {
    if (val && val[0] != '\0')
      payload += ",\"" + std::string(key) + "\":\"" + escape_json_str(std::string(val)) + "\"";
  };

  auto fmt_double = [](double v) -> std::string {
    char buf[32];
    if (v == static_cast<double>(static_cast<long long>(v))) {
      snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
      snprintf(buf, sizeof(buf), "%.6f", v);
      char* dot = strchr(buf, '.');
      if (dot) {
        char* end = buf + strlen(buf) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (*end == '.') *end = '\0';
      }
    }
    return std::string(buf);
  };

  if (strcmp(domain, "sensor") == 0) {
    payload  = "{\"name\":\"" + escape_json_str(std::string(name)) + "\"";
    payload += ",\"state_topic\":\"" + state_topic + "\"";
    payload += ",\"unique_id\":\"" + unique_id + "\"";
    add_field("value_template", vt);
    add_field("unit_of_measurement", unit);
    add_field("device_class", dc);
    add_field("state_class", sc);
    payload += ",\"device\":" + device_json + "}";

  } else if (strcmp(domain, "binary_sensor") == 0) {
    payload  = "{\"name\":\"" + escape_json_str(std::string(name)) + "\"";
    payload += ",\"state_topic\":\"" + state_topic + "\"";
    payload += ",\"unique_id\":\"" + unique_id + "\"";
    add_field("value_template", vt);
    payload += ",\"payload_on\":\"01\",\"payload_off\":\"00\"";
    add_field("device_class", dc);
    payload += ",\"device\":" + device_json + "}";

  } else if (strcmp(domain, "switch") == 0) {
    payload  = "{\"name\":\"" + escape_json_str(std::string(name)) + "\"";
    payload += ",\"state_topic\":\"" + state_topic + "\"";
    payload += ",\"command_topic\":\"" + command_topic + "\"";
    payload += ",\"unique_id\":\"" + unique_id + "\"";
    add_field("value_template", vt);
    payload += ",\"state_on\":\"01\",\"state_off\":\"00\"";
    payload += ",\"payload_on\":\"01\",\"payload_off\":\"00\"";
    payload += ",\"device\":" + device_json + "}";

  } else if (strcmp(domain, "select") == 0) {
    payload  = "{\"name\":\"" + escape_json_str(std::string(name)) + "\"";
    payload += ",\"state_topic\":\"" + state_topic + "\"";
    payload += ",\"command_topic\":\"" + command_topic + "\"";
    payload += ",\"unique_id\":\"" + unique_id + "\"";
    add_field("value_template", vt);
    add_field("command_template", ct);
    if (opts[0] != '\0') payload += ",\"options\":" + std::string(opts);
    payload += ",\"device\":" + device_json + "}";

  } else if (strcmp(domain, "number") == 0) {
    cJSON* dt_item = cJSON_GetObjectItemCaseSensitive(root, "dt");
    cJSON* sf_item = cJSON_GetObjectItemCaseSensitive(root, "sf");
    const char* dtype  = (dt_item && cJSON_IsString(dt_item)) ? dt_item->valuestring : "uint8";
    int scale_factor   = (sf_item && cJSON_IsNumber(sf_item)) ? static_cast<int>(sf_item->valuedouble) : 1;
    if (scale_factor < 1) scale_factor = 1;

    double type_min, type_max;
    if      (strcmp(dtype, "int8")   == 0) { type_min = -128.0;        type_max = 127.0; }
    else if (strcmp(dtype, "int16")  == 0) { type_min = -32768.0;      type_max = 32767.0; }
    else if (strcmp(dtype, "int24")  == 0) { type_min = -8388608.0;    type_max = 8388607.0; }
    else if (strcmp(dtype, "int32")  == 0) { type_min = -2147483648.0; type_max = 2147483647.0; }
    else if (strcmp(dtype, "uint8")  == 0) { type_min = 0.0;           type_max = 255.0; }
    else if (strcmp(dtype, "uint16") == 0) { type_min = 0.0;           type_max = 65535.0; }
    else if (strcmp(dtype, "uint24") == 0) { type_min = 0.0;           type_max = 16777215.0; }
    else                                   { type_min = 0.0;           type_max = 4294967295.0; }

    double min_val  = type_min / scale_factor;
    double max_val  = type_max / scale_factor;
    double step_val = (scale_factor > 1) ? (1.0 / scale_factor) : 1.0;

    payload  = "{\"name\":\"" + escape_json_str(std::string(name)) + "\"";
    payload += ",\"state_topic\":\"" + state_topic + "\"";
    payload += ",\"command_topic\":\"" + command_topic + "\"";
    payload += ",\"unique_id\":\"" + unique_id + "\"";
    add_field("value_template", vt);
    add_field("command_template", ct);
    add_field("unit_of_measurement", unit);
    add_field("device_class", dc);
    payload += ",\"mode\":\"box\"";
    payload += ",\"min\":"  + fmt_double(min_val);
    payload += ",\"max\":"  + fmt_double(max_val);
    payload += ",\"step\":" + fmt_double(step_val);
    payload += ",\"device\":" + device_json + "}";

  } else if (strcmp(domain, "button") == 0) {
    payload  = "{\"name\":\"" + escape_json_str(std::string(name)) + "\"";
    payload += ",\"command_topic\":\"" + command_topic + "\"";
    payload += ",\"unique_id\":\"" + unique_id + "\"";
    payload += ",\"payload_press\":\"01\"";
    add_field("device_class", dc);
    payload += ",\"device\":" + device_json + "}";

  } else {
    cJSON_Delete(root);
    return false;
  }

  std::string topic_key = erd_id_str;
  if (field_id[0] != '\0') { topic_key += "_"; topic_key += field_id; }
  std::string topic = "homeassistant/" + std::string(domain)
                       + "/" + device_id + "/" + topic_key + "/config";

  cJSON_Delete(root);

  auto* item = new (std::nothrow) HaDiscoveryItem{std::move(topic), std::move(payload)};
  if (!item) return false;
  if (xQueueSend(this->ha_discovery_queue_, &item, portMAX_DELAY) != pdTRUE) {
    delete item;
    return false;
  }
  return true;
}

#endif  // USE_ESP_IDF

}  // namespace geappliances_bridge
}  // namespace esphome
