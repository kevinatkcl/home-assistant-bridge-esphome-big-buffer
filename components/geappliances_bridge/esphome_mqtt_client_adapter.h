#pragma once

#include <string>
#include <deque>
#include <set>

extern "C" {
#include "i_mqtt_client.h"
#include "tiny_event.h"
}

struct PendingErdUpdate {
  std::string topic;
  std::string payload;
};

typedef struct {
  i_mqtt_client_t interface;
  std::string* device_id;
  tiny_event_t on_write_request_event;
  tiny_event_t on_mqtt_disconnect_event;
  std::deque<PendingErdUpdate>* pending_updates;
  // Optional filter: when non-null, update_erd only publishes ERDs that are
  // present in this set. Used when appliance_api_parsing is enabled.
  const std::set<tiny_erd_t>* valid_erds_filter;
  // Optional set of string-type ERDs: when an ERD is in this set, update_erd
  // publishes the raw bytes as a null-terminated ASCII string instead of hex.
  const std::set<tiny_erd_t>* string_erds_filter;
} esphome_mqtt_client_adapter_t;

#ifdef __cplusplus
extern "C" {
#endif

void esphome_mqtt_client_adapter_init(
  esphome_mqtt_client_adapter_t* self,
  const char* device_id);

void esphome_mqtt_client_adapter_set_valid_erds_filter(
  esphome_mqtt_client_adapter_t* self,
  const std::set<tiny_erd_t>* valid_erds_filter);

void esphome_mqtt_client_adapter_set_string_erds_filter(
  esphome_mqtt_client_adapter_t* self,
  const std::set<tiny_erd_t>* string_erds_filter);

void esphome_mqtt_client_adapter_notify_disconnected(
  esphome_mqtt_client_adapter_t* self);

void esphome_mqtt_client_adapter_notify_connected(
  esphome_mqtt_client_adapter_t* self);

void esphome_mqtt_client_adapter_destroy(
  esphome_mqtt_client_adapter_t* self);

#ifdef __cplusplus
}
#endif
