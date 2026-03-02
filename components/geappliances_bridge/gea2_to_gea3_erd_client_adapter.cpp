/*!
 * @file
 * @brief GEA2 to GEA3 ERD client adapter implementation.
 */

#include "gea2_to_gea3_erd_client_adapter.h"

static bool adapter_read(
  i_tiny_gea3_erd_client_t* _self,
  tiny_gea3_erd_client_request_id_t* request_id,
  uint8_t address,
  tiny_erd_t erd)
{
  auto self = reinterpret_cast<gea2_to_gea3_erd_client_adapter_t*>(_self);
  return tiny_gea2_erd_client_read(
    self->gea2_client,
    reinterpret_cast<tiny_gea2_erd_client_request_id_t*>(request_id),
    address,
    erd);
}

static bool adapter_write(
  i_tiny_gea3_erd_client_t* _self,
  tiny_gea3_erd_client_request_id_t* request_id,
  uint8_t address,
  tiny_erd_t erd,
  const void* data,
  uint8_t data_size)
{
  auto self = reinterpret_cast<gea2_to_gea3_erd_client_adapter_t*>(_self);
  return tiny_gea2_erd_client_write(
    self->gea2_client,
    reinterpret_cast<tiny_gea2_erd_client_request_id_t*>(request_id),
    address,
    erd,
    data,
    data_size);
}

static bool adapter_subscribe(i_tiny_gea3_erd_client_t* _self, uint8_t address)
{
  (void)_self;
  (void)address;
  return false; // GEA2 does not expose subscriptions via i_tiny_gea2_erd_client_t
}

static bool adapter_retain_subscription(i_tiny_gea3_erd_client_t* _self, uint8_t address)
{
  (void)_self;
  (void)address;
  return false;
}

static i_tiny_event_t* adapter_on_activity(i_tiny_gea3_erd_client_t* _self)
{
  auto self = reinterpret_cast<gea2_to_gea3_erd_client_adapter_t*>(_self);
  return &self->translated_event.interface;
}

static const i_tiny_gea3_erd_client_api_t api = {
  adapter_read,
  adapter_write,
  adapter_subscribe,
  adapter_retain_subscription,
  adapter_on_activity
};

extern "C" void gea2_to_gea3_erd_client_adapter_init(
  gea2_to_gea3_erd_client_adapter_t* self,
  i_tiny_gea2_erd_client_t* gea2_client)
{
  self->interface.api = &api;
  self->gea2_client = gea2_client;
  tiny_event_init(&self->translated_event);

  // Re-publish GEA2 activity as GEA3 activity. The structs are binary-compatible
  // for activity types 0-3 (read/write), so a direct re-publish is safe.
  tiny_event_subscription_init(
    &self->gea2_sub, self,
    +[](void* context, const void* args) {
      auto self = reinterpret_cast<gea2_to_gea3_erd_client_adapter_t*>(context);
      tiny_event_publish(&self->translated_event, args);
    });
  tiny_event_subscribe(tiny_gea2_erd_client_on_activity(gea2_client), &self->gea2_sub);
}
