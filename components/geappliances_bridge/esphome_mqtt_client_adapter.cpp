#include "esphome_mqtt_client_adapter.h"
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

extern "C" {
#include "tiny_utils.h"
#include "tiny_event.h"
}

#include <cstdio>
#include <string>
#include <cctype>
#include <map>

static const char *const TAG = "geappliances_bridge.mqtt";

// Maximum number of distinct ERDs that can be pending (safety bound — in
// practice bounded by the number of ERDs the appliance registers, typically <100).
static constexpr size_t MAX_PENDING_UPDATES = 200;

// Maximum number of pending ERD updates flushed to MQTT in a single
// notify_connected() / loop() drain call.  Keeping this small (≤5) ensures
// the main loop() does not stall while the IDF MQTT client's API mutex is
// held by the MQTT task sending previous PUBLISH packets.  At 5 per call and
// a typical loop rate of ~200 Hz, 200 pending updates drain in ≤200 ms.
static constexpr size_t MAX_FLUSH_PER_CALL = 5;

// How long to wait after an MQTT (re)connect before starting to drain the
// pending write-topic subscribe() queue.  When the MQTT client uses persistent
// sessions (clean_session=false) and the device has been through a crash loop,
// the broker accumulates many queued QoS 1/2 messages and delivers them all at
// once on the next connect — exhausting the client's receive-maximum quota
// instantly (logged by Mosquitto as "has exceeded its receive-maximum quota").
// Processing this backlog keeps the IDF MQTT task holding its API mutex for
// several seconds, making every subscribe() call block for 800+ ms.  Delaying
// subscribe() draining by MQTT_SETTLE_DELAY_MS gives the IDF MQTT task time to
// clear the broker's backlog before we add new subscriptions.
//
// Note: the correct long-term fix is to configure the MQTT client with
// clean_session: true in the ESPHome YAML so no messages accumulate between
// sessions.  This delay is an in-firmware defence-in-depth measure.
static constexpr uint32_t MQTT_SETTLE_DELAY_MS = 2000;

// Minimum interval between successive subscribe() calls (after the settle
// period).  Even when the IDF MQTT stack is healthy, each subscribe() call
// acquires the IDF MQTT API mutex; keeping at least SUBSCRIBE_MIN_INTERVAL_MS
// between calls ensures the MQTT task has time to send the SUBSCRIBE packet
// and process the SUBACK before the next call arrives, preventing the outbox
// from filling and subsequent calls from blocking.
static constexpr uint32_t SUBSCRIBE_MIN_INTERVAL_MS = 100;

static std::string build_topic(esphome_mqtt_client_adapter_t* self, const char* suffix)
{
  return std::string("geappliances/") + *self->device_id + suffix;
}

static void register_erd(i_mqtt_client_t* _self, tiny_erd_t erd)
{
  auto self = reinterpret_cast<esphome_mqtt_client_adapter_t*>(_self);

  // Track which ERDs the device registers so the bridge can filter
  // HA discovery entities to only those actually supported by the device.
  if (self->registered_erds_out != nullptr) {
    self->registered_erds_out->insert(erd);
  }

  ESP_LOGD(TAG, "Registered ERD 0x%04X", erd);

  // Only subscribe once per ERD lifetime. The polling bridge clears its own
  // erd_set on every MQTT disconnect so that it can rebuild the polling list,
  // which causes register_erd() to be called again for every ERD on each
  // reconnect. ESPHome's MQTT client re-establishes all previous subscriptions
  // automatically on reconnect, so calling subscribe() again would leak a new
  // closure and register a duplicate write-command handler per reconnect cycle.
  //
  // IMPORTANT: this guard must come before any heap allocation so that the
  // fast path (already subscribed) avoids all dynamic memory operations. On
  // reconnect, register_erd() is called for every ERD; allocating strings and
  // then immediately freeing them on 100+ ERDs causes severe heap fragmentation
  // that can exhaust memory over multiple reconnect cycles.
  if (self->subscribed_write_erds != nullptr &&
      self->subscribed_write_erds->count(erd)) {
    return;
  }
  if (self->subscribed_write_erds != nullptr) {
    self->subscribed_write_erds->insert(erd);
  }

  // Build the write-topic string only for ERDs that actually need a new
  // subscription (i.e. first time seen). Combine the suffix and "/write"
  // in a single snprintf to avoid an intermediate std::string allocation.
  char topic_suffix[40];
  snprintf(topic_suffix, sizeof(topic_suffix), "/erd/0x%04x/write", erd);
  std::string write_topic = build_topic(self, topic_suffix);

  // Defer the actual mqtt_client->subscribe() call to the next drain cycle
  // (esphome_mqtt_client_adapter_drain_subscribe, called once per loop()
  // iteration).  Calling subscribe() synchronously here would stall the
  // current timer callback for the full duration of the IDF MQTT API mutex
  // acquisition; when register_erd() is called 31+ times in a single
  // HSM state-entry (e.g. for custom ERDs in state_polling) that adds up to
  // 1–2 seconds and triggers ESPHome's "took a long time" warning.
  if (self->pending_subscriptions != nullptr) {
    self->pending_subscriptions->emplace_back(erd, std::move(write_topic));
  }
}

static void update_erd(i_mqtt_client_t* _self, tiny_erd_t erd, const void* value, uint8_t size)
{
  auto self = reinterpret_cast<esphome_mqtt_client_adapter_t*>(_self);

  // If a valid ERD filter is set (appliance_api_parsing mode), skip ERDs not
  // in the validated list. This applies to both subscription and polling modes.
  if(self->valid_erds_filter != nullptr &&
     self->valid_erds_filter->find(erd) == self->valid_erds_filter->end()) {
    return;
  }

  // Validate inputs
  if (value == nullptr || size == 0) {
    ESP_LOGW(TAG, "Invalid ERD update: null value or zero size for ERD 0x%04X", erd);
    return;
  }
  
  char topic_suffix[32];
  snprintf(topic_suffix, sizeof(topic_suffix), "/erd/0x%04x/value", erd);
  std::string topic = build_topic(self, topic_suffix);

  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(value);

  // String-type ERDs: publish the raw bytes as a null-terminated ASCII string
  // instead of a hex string so Home Assistant displays human-readable text.
  bool is_string = (self->string_erds_filter != nullptr &&
                    self->string_erds_filter->find(erd) != self->string_erds_filter->end());

  std::string payload;
  if (is_string) {
    // Reserve only up to the first null byte (or full size if no null found)
    uint8_t str_len = 0;
    while (str_len < size && bytes[str_len] != 0) str_len++;
    payload.reserve(str_len);
    for (uint8_t i = 0; i < str_len; i++) {
      if (isprint(bytes[i])) {
        payload += static_cast<char>(bytes[i]);
      } else {
        ESP_LOGD(TAG, "ERD 0x%04X: skipping non-printable byte 0x%02X at offset %u",
                 erd, bytes[i], i);
      }
    }
  } else {
    // Convert binary data to hex string
    payload.reserve(size * 2);
    for (uint8_t i = 0; i < size; i++) {
      char hex[3];
      snprintf(hex, sizeof(hex), "%02x", bytes[i]);
      payload += hex;
    }
  }
  
  // Publish to MQTT or queue if not connected
  auto mqtt_client = esphome::mqtt::global_mqtt_client;
  if (mqtt_client != nullptr && mqtt_client->is_connected()) {
    mqtt_client->publish(topic, payload, 0, true);  // QoS 0, retain
  } else {
    // Queue the update for later when MQTT connects. The map key is the ERD so
    // a repeated update overwrites the previous pending value instead of
    // appending — this prevents the queue from filling with stale duplicates
    // across multiple polling cycles while MQTT is down.
    if (self->pending_updates != nullptr && self->pending_updates->size() < MAX_PENDING_UPDATES) {
      (*self->pending_updates)[erd] = {topic, payload};
      ESP_LOGD(TAG, "MQTT not connected, queued ERD update for 0x%04X (queue size: %zu)",
               erd, self->pending_updates->size());
    } else if (self->pending_updates == nullptr) {
      ESP_LOGW(TAG, "Pending updates queue not initialized, dropping ERD update for 0x%04X", erd);
    } else {
      ESP_LOGW(TAG, "Pending update queue full, dropping ERD update for 0x%04X", erd);
    }
  }
}

static void update_erd_write_result(
  i_mqtt_client_t* _self,
  tiny_erd_t erd,
  bool success,
  tiny_gea3_erd_client_write_failure_reason_t failure_reason)
{
  auto self = reinterpret_cast<esphome_mqtt_client_adapter_t*>(_self);
  
  char topic_suffix[48];
  snprintf(topic_suffix, sizeof(topic_suffix), "/erd/0x%04x/write_result", erd);
  std::string topic = build_topic(self, topic_suffix);
  
  std::string payload = success ? "success" : "failure";
  if (!success) {
    char reason[16];
    snprintf(reason, sizeof(reason), " (reason: %d)", failure_reason);
    payload += reason;
  }
  
  auto mqtt_client = esphome::mqtt::global_mqtt_client;
  if (mqtt_client != nullptr && mqtt_client->is_connected()) {
    mqtt_client->publish(topic, payload, 0, false);  // QoS 0, no retain
  } else {
    ESP_LOGD(TAG, "MQTT not connected, skipping write result for 0x%04X", erd);
  }
  
  ESP_LOGD(TAG, "Write result for ERD 0x%04X: %s", erd, payload.c_str());
}

static i_tiny_event_t* on_write_request(i_mqtt_client_t* _self)
{
  auto self = reinterpret_cast<esphome_mqtt_client_adapter_t*>(_self);
  return &self->on_write_request_event.interface;
}

static i_tiny_event_t* on_mqtt_disconnect(i_mqtt_client_t* _self)
{
  auto self = reinterpret_cast<esphome_mqtt_client_adapter_t*>(_self);
  return &self->on_mqtt_disconnect_event.interface;
}

static const i_mqtt_client_api_t api = {
  register_erd,
  update_erd,
  update_erd_write_result,
  on_write_request,
  on_mqtt_disconnect
};

extern "C" void esphome_mqtt_client_adapter_init(
  esphome_mqtt_client_adapter_t* self,
  const char* device_id)
{
  self->interface.api = &api;
  self->device_id = new std::string(device_id);
  self->pending_updates = new std::map<tiny_erd_t, PendingErdUpdate>();
  self->valid_erds_filter = nullptr;
  self->string_erds_filter = nullptr;
  self->registered_erds_out = nullptr;
  self->subscribed_write_erds = new std::set<tiny_erd_t>();
  self->pending_subscriptions = new std::vector<std::pair<tiny_erd_t, std::string>>();
  self->mqtt_connected_at_ms  = 0;
  self->last_subscribe_ms     = 0;

  tiny_event_init(&self->on_write_request_event);
  tiny_event_init(&self->on_mqtt_disconnect_event);
}

extern "C" void esphome_mqtt_client_adapter_set_valid_erds_filter(
  esphome_mqtt_client_adapter_t* self,
  const std::set<tiny_erd_t>* valid_erds_filter)
{
  self->valid_erds_filter = valid_erds_filter;
}

extern "C" void esphome_mqtt_client_adapter_set_string_erds_filter(
  esphome_mqtt_client_adapter_t* self,
  const std::set<tiny_erd_t>* string_erds_filter)
{
  self->string_erds_filter = string_erds_filter;
}

extern "C" void esphome_mqtt_client_adapter_set_registered_erds_out(
  esphome_mqtt_client_adapter_t* self,
  std::set<tiny_erd_t>* registered_erds_out)
{
  self->registered_erds_out = registered_erds_out;
}

extern "C" void esphome_mqtt_client_adapter_notify_disconnected(
  esphome_mqtt_client_adapter_t* self)
{
  // Reset the connect timestamp so drain_subscribe() restarts the settle
  // delay on the next reconnect, and the broker's re-delivered message
  // backlog has time to drain before subscribe() is called again.
  self->mqtt_connected_at_ms = 0;
  // Publish the disconnect event to notify the bridge
  // This will clear the ERD registry and trigger resubscription
  tiny_event_publish(&self->on_mqtt_disconnect_event, nullptr);
}

extern "C" void esphome_mqtt_client_adapter_notify_connected(
  esphome_mqtt_client_adapter_t* self)
{
  // Record the time of first connection after each disconnect (or initial
  // boot).  drain_subscribe() uses this as the start of the settle window.
  //
  // Thread-safety: notify_connected() is only ever called from loop() on the
  // main ESPHome task (see geappliances_bridge.cpp).  notify_disconnected()
  // is similarly dispatched through the main loop.  No concurrent access to
  // mqtt_connected_at_ms is possible, so no mutex is needed.
  if (self->mqtt_connected_at_ms == 0) {
    self->mqtt_connected_at_ms = esphome::millis();
    ESP_LOGI(TAG, "MQTT connected; subscribe drain will begin after %u ms settle delay",
             MQTT_SETTLE_DELAY_MS);
  }

  // Flush up to MAX_FLUSH_PER_CALL pending ERD updates per call.
  // loop() calls this every iteration while MQTT is connected so the full
  // backlog drains across multiple loop cycles without stalling the loop.
  if (self->pending_updates == nullptr || self->pending_updates->empty()) {
    return;
  }
  auto mqtt_client = esphome::mqtt::global_mqtt_client;
  if (mqtt_client == nullptr || !mqtt_client->is_connected()) {
    return;
  }
  size_t flushed = 0;
  // Publish-then-erase is intentional: we must publish before erasing so that
  // the iterator (and the topic/payload strings it references) remains valid
  // during the publish() call.  A range-erase would invalidate iterators.
  while (!self->pending_updates->empty() && flushed < MAX_FLUSH_PER_CALL) {
    auto it = self->pending_updates->begin();
    mqtt_client->publish(it->second.topic, it->second.payload, 0, true);  // QoS 0, retain
    self->pending_updates->erase(it);
    flushed++;
  }
  if (flushed > 0 && self->pending_updates->empty()) {
    ESP_LOGI(TAG, "Flushed all pending ERD updates");
  }
}

extern "C" bool esphome_mqtt_client_adapter_drain_subscribe(
  esphome_mqtt_client_adapter_t* self)
{
  if (self->pending_subscriptions == nullptr || self->pending_subscriptions->empty()) {
    return false;
  }

  // Enforce the post-connect settle delay.  When MQTT reconnects with a
  // persistent session (clean_session=false) the broker may queue hundreds of
  // unacknowledged QoS 1/2 messages and deliver them all at once, immediately
  // saturating the client's receive-maximum quota (visible in the Mosquitto
  // log as "has exceeded its receive-maximum quota").  The IDF MQTT task is
  // then busy for several seconds processing PUBACKs, holding its API mutex.
  // Any subscribe() call during that window blocks for 800+ ms, starving
  // run_protocol_stack_() and causing the "took a long time" warnings seen
  // in the field.  Waiting MQTT_SETTLE_DELAY_MS lets the IDF MQTT task drain
  // the broker's backlog before we begin adding new subscriptions.
  if (self->mqtt_connected_at_ms == 0) {
    // Not yet connected; nothing to subscribe to yet.
    return !self->pending_subscriptions->empty();
  }
  uint32_t now = esphome::millis();
  // Unsigned subtraction wraps correctly at the ~49-day millis() rollover
  // (standard Arduino/ESP32 pattern): e.g. if now=500 and mqtt_connected_at_ms
  // was set 1000 ms before rollover, (uint32_t)(500 - (UINT32_MAX-999)) = 1500.
  if (now - self->mqtt_connected_at_ms < MQTT_SETTLE_DELAY_MS) {
    return !self->pending_subscriptions->empty();
  }

  // Rate-limit: allow at most one subscribe() call per SUBSCRIBE_MIN_INTERVAL_MS.
  // Even after the settle period, each subscribe() call acquires the IDF MQTT
  // API mutex; spacing the calls out ensures the MQTT task can send the
  // SUBSCRIBE packet and receive the SUBACK before the next call arrives.
  // Unsigned subtraction wraps correctly at rollover (same as above).
  if (self->last_subscribe_ms != 0 &&
      now - self->last_subscribe_ms < SUBSCRIBE_MIN_INTERVAL_MS) {
    return !self->pending_subscriptions->empty();
  }

  // Extract the front entry (oldest pending subscription) before erasing,
  // to avoid calling front() twice.
  auto& front_entry = self->pending_subscriptions->front();
  tiny_erd_t  erd        = front_entry.first;
  std::string write_topic = std::move(front_entry.second);
  self->pending_subscriptions->erase(self->pending_subscriptions->begin());

  auto mqtt_client = esphome::mqtt::global_mqtt_client;
  if (mqtt_client == nullptr) {
    return !self->pending_subscriptions->empty();
  }

  self->last_subscribe_ms = now;

  // Subscribe to the write topic.  QoS 0 is sufficient: write commands are
  // user-initiated one-shots and the broker does not retain them.
  mqtt_client->subscribe(
    write_topic,
    [self, erd](const std::string& /*topic*/, const std::string& payload) {
      ESP_LOGD(TAG, "Write request for ERD 0x%04X: %s", erd, payload.c_str());

      if (payload.length() % 2 != 0) {
        ESP_LOGW(TAG, "Invalid hex payload for ERD 0x%04X: odd length (%zu)", erd, payload.length());
        return;
      }

      std::vector<uint8_t> data;
      data.reserve(payload.length() / 2);
      for (size_t i = 0; i < payload.length(); i += 2) {
        char byte_str[3] = {payload[i], payload[i + 1], '\0'};
        if (!std::isxdigit(static_cast<unsigned char>(payload[i])) ||
            !std::isxdigit(static_cast<unsigned char>(payload[i + 1]))) {
          ESP_LOGW(TAG, "Invalid hex characters in payload for ERD 0x%04X at position %zu", erd, i);
          return;
        }
        data.push_back(static_cast<uint8_t>(strtol(byte_str, nullptr, 16)));
      }

      if (data.empty() || data.size() > 255) {
        ESP_LOGW(TAG, "Invalid data size for ERD 0x%04X: %zu bytes", erd, data.size());
        return;
      }

      mqtt_client_on_write_request_args_t args = {
        .erd  = erd,
        .size = static_cast<uint8_t>(data.size()),
        .value = data.data()
      };
      tiny_event_publish(&self->on_write_request_event, &args);
    },
    0  // QoS 0
  );
  return !self->pending_subscriptions->empty();
}

extern "C" void esphome_mqtt_client_adapter_destroy(
  esphome_mqtt_client_adapter_t* self)
{
  if (self->device_id != nullptr) {
    delete self->device_id;
    self->device_id = nullptr;
  }
  if (self->pending_updates != nullptr) {
    delete self->pending_updates;
    self->pending_updates = nullptr;
  }
  if (self->subscribed_write_erds != nullptr) {
    delete self->subscribed_write_erds;
    self->subscribed_write_erds = nullptr;
  }
  if (self->pending_subscriptions != nullptr) {
    delete self->pending_subscriptions;
    self->pending_subscriptions = nullptr;
  }
}
