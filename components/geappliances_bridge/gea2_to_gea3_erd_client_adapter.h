/*!
 * @file
 * @brief Adapter that presents a GEA2 ERD client as a GEA3 ERD client interface.
 *
 * GEA2 and GEA3 share the same read/write activity layout (activity types 0-3 and
 * their union members are binary-compatible), so a single activity handler can serve
 * both protocols.  The adapter owns a translated event that re-publishes GEA2 activity
 * as GEA3 activity, so callers can subscribe to one event regardless of protocol.
 *
 * Subscribe operations are forwarded as no-ops (GEA2 does not expose subscriptions
 * through i_tiny_gea2_erd_client_t); in AUTO mode the bridge falls back to polling.
 */

#pragma once

#include "i_tiny_gea2_erd_client.h"
#include "i_tiny_gea3_erd_client.h"
#include "tiny_event.h"
#include "tiny_event_subscription.h"

typedef struct {
  i_tiny_gea3_erd_client_t interface;
  i_tiny_gea2_erd_client_t* gea2_client;
  tiny_event_t translated_event;
  tiny_event_subscription_t gea2_sub;
} gea2_to_gea3_erd_client_adapter_t;

#ifdef __cplusplus
extern "C" {
#endif

void gea2_to_gea3_erd_client_adapter_init(
  gea2_to_gea3_erd_client_adapter_t* self,
  i_tiny_gea2_erd_client_t* gea2_client);

#ifdef __cplusplus
}
#endif
