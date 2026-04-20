# Home Assistant MQTT Discovery — Implementation Guide

> **Audience**: Developers working on the `geappliances_bridge` ESPHome component.

---

## Table of Contents

1. [Overview](#1-overview)
2. [High-Level Flow](#2-high-level-flow)
3. [Discovery Readiness Gating](#3-discovery-readiness-gating)
4. [Polling State Machine Integration](#4-polling-state-machine-integration)
5. [AUTO-Mode Edge Case](#5-auto-mode-edge-case)
6. [Entity Definitions — JSONL Fetch at Runtime](#6-entity-definitions--jsonl-fetch-at-runtime)
7. [JSONL Format Reference](#7-jsonl-format-reference)
8. [Entity Publishing Pipeline](#8-entity-publishing-pipeline)
9. [ERD Filtering](#9-erd-filtering)
10. [Multi-Field ERDs](#10-multi-field-erds)
11. [Configuration Parameters](#11-configuration-parameters)
12. [Lessons Learned](#12-lessons-learned)
13. [Known Limitations](#13-known-limitations)

---

## 1. Overview

When the GE Appliances Bridge connects to an appliance and fully enumerates its supported ERDs (Entity Reference Designators), it automatically publishes [Home Assistant MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery) payloads so that entities (sensors, switches, selects, numbers, binary sensors, buttons) appear in HA without any manual YAML configuration.

The feature is controlled by the YAML option `generate_device_config` (default: `false`). It is only compiled for ESP-IDF targets because it requires `esp_http_client`, `cJSON`, and FreeRTOS.

---

## 2. High-Level Flow

```
Bridge connects to appliance
         │
         ▼
register_erd() callbacks populate ha_registered_erds_ (set<uint16_t>)
         │
         ▼
Discovery readiness gate (see §3)
─────────────────────────────────────────────────────
Polling mode  →  wait for mqtt_bridge_polling_.polling_list_complete = true
AUTO mode     →  if subscription active:  10 s ERD-silence quiet window
               →  if no subscription:     wait for polling_list_complete
Subscribe mode→  10 s ERD-silence quiet window
─────────────────────────────────────────────────────
         │
         ▼
publish_ha_discovery_() creates FreeRTOS queue + spawns ha_fetch task (priority 1)
         │
         ▼
ha_fetch task determines which JSONL category files are needed,
downloads each file over HTTPS, parses line-by-line,
discards entities for unregistered ERDs,
enqueues matching HaDiscoveryItem* pointers
         │
         ▼
Main loop() dequeues one item per HA_ENTITY_PUBLISH_INTERVAL_MS (50 ms),
publishes to homeassistant/<domain>/<device_id>/<erd_id>/config (QoS 0, retained)
         │
         ▼
ha_fetch sends nullptr sentinel → main loop marks publishing complete
```

---

## 3. Discovery Readiness Gating

The readiness check runs every `loop()` iteration while `ha_discovery_pending_ = true`:

```cpp
bool is_poll_mode = !(mode_ == BRIDGE_MODE_SUBSCRIBE ||
                      (mode_ == BRIDGE_MODE_AUTO && subscription_mode_active_));

bool ready = false;
if (is_poll_mode) {
    // Definitive signal: state machine entered state_polling
    ready = mqtt_bridge_polling_.polling_list_complete;
} else {
    // Subscription mode: quiet window since last new ERD seen
    bool subscription_confirmed = (mode_ == BRIDGE_MODE_SUBSCRIBE) ||
                                  subscription_activity_detected_;
    if (subscription_confirmed) {
        ready = (millis() - ha_discovery_last_activity_ >= HA_DISCOVERY_QUIET_MS);
    }
}
```

`HA_DISCOVERY_QUIET_MS` is 10,000 ms. `ha_discovery_last_activity_` is reset every time a new ERD subscription publication arrives that hasn't been seen before.

---

## 4. Polling State Machine Integration

The polling bridge (`mqtt_bridge_polling_t`) has a hierarchical state machine that walks through:

```
state_identify_appliance
    → state_add_common_erds
        → state_add_energy_erds
            → state_add_appliance_erds
                → state_polling   ← ERD enumeration complete
```

Two new fields were added to `mqtt_bridge_polling_t`:

| Field | Type | Purpose |
|-------|------|---------|
| `polling_list_complete` | `bool` | Set `true` when `state_polling` is entered; reset to `false` when `state_identify_appliance` is entered (appliance reconnect) |
| `current_state_name` | `const char*` | Human-readable state name for logging; lets `geappliances_bridge.cpp` emit debug messages without depending on ESPHome log headers inside `mqtt_bridge.cpp` |

`mqtt_bridge.cpp` sets `current_state_name` and `polling_list_complete` at every state entry. The bridge's `loop()` detects transitions by comparing `current_state_name` to a cached `last_polling_state_name_`, and emits:

```
[D] geappliances_bridge: Polling bridge state: add_energy_erds (ERDs registered: 3)
[D] geappliances_bridge: Polling bridge state: state_polling (ERDs registered: 12)
```

**Why not use a timer?** Failed ERD reads during discovery phases trigger retries spaced by the GEA request timeout + retry count (up to several seconds per ERD). A 10 s quiet timer could fire mid-enumeration if a run of slow reads happened to space themselves apart by ≥10 s. The `state_polling` entry flag is the only reliable signal.

---

## 5. AUTO-Mode Edge Case

In `BRIDGE_MODE_AUTO`, the bridge initially starts in subscription mode. If no subscription publications arrive within 30 s, it falls back to polling. The discovery gate must handle this correctly:

- **With subscription activity**: `subscription_activity_detected_ = true` → normal 10 s quiet window applies.
- **Without subscription activity**: `subscription_activity_detected_` stays `false` → the quiet-window branch is **suppressed**. Once the 30 s watchdog fires and `subscription_mode_active_` becomes `false`, `is_poll_mode` becomes `true` and the `polling_list_complete` check takes over.

Without this guard, the original code would fire discovery 10 s after bridge init (the quiet window would start counting immediately from `ha_discovery_last_activity_` initialized in `initialize_mqtt_bridge_()`) — long before the polling bridge had enumerated any ERDs.

---

## 6. Entity Definitions — JSONL Fetch at Runtime

The full ERD entity table (12,728+ entries) was originally compiled into flash as a C header file. That approach exceeded the ESP32-C3's 1.8 MB flash limit. The solution: store definitions in compact JSONL files in the repository and download only the needed categories at runtime.

### Category files

| File | ERD range | Contents |
|------|-----------|----------|
| `ha_discovery/common.jsonl` | `0x0000–0x0FFF` | Cross-appliance ERDs (model, serial, WiFi, etc.) |
| `ha_discovery/refrigeration.jsonl` | `0x1000–0x1FFF` | Refrigerator/freezer ERDs |
| `ha_discovery/laundry.jsonl` | `0x2000–0x2FFF` | Washer/dryer ERDs |
| `ha_discovery/dishwasher.jsonl` | `0x3000–0x3FFF` | Dishwasher ERDs |
| `ha_discovery/waterheater.jsonl` | `0x4000–0x4FFF` | Water heater ERDs |
| `ha_discovery/range.jsonl` | `0x5000–0x5FFF` | Oven/range ERDs |
| `ha_discovery/airconditioning.jsonl` | `0x7000–0x7FFF` | Air conditioner / Zoneline ERDs |
| `ha_discovery/waterfilter.jsonl` | `0x8000–0x8FFF` | Water filter ERDs |
| `ha_discovery/smallappliance.jsonl` | `0x9000–0x9FFF` | Small appliance ERDs |
| `ha_discovery/energy.jsonl` | `0xD000–0xDFFF` | Energy monitoring ERDs |

`common.jsonl` is always fetched. The other files are fetched only if the device registered at least one ERD in that range.

### Generation

The JSONL files are generated by `scripts/generate_erd_lists.py` from the `public-appliance-api-documentation` submodule. Run `make generate` to regenerate after a submodule update.

### Runtime fetch

```
ha_fetch (FreeRTOS task, stack 12 KB, priority 1)
    │
    ├── build list of needed categories from ha_registered_erds_
    ├── build shared device JSON blob (identifiers, name, manufacturer, model, serial)
    │
    └── for each needed category:
            esp_http_client GET over HTTPS (crt_bundle for certificate verification)
            → stream response in 4 KB chunks
            → split by newline into per-line buffer
            → cJSON_Parse each line
            → check if entity's ERD is in ha_registered_erds_
            → if yes: build HA discovery payload, heap-allocate HaDiscoveryItem, xQueueSend
            → vTaskDelay(50 ms) between category requests
    │
    └── send nullptr sentinel to signal completion
```

**Priority 1** ensures the IDF MQTT task (priority 5) always preempts the fetch task when processing inbound broker events.

---

## 7. JSONL Format Reference

Each line in a `.jsonl` file is a compact JSON object:

| Field | Key | Type | Description |
|-------|-----|------|-------------|
| ERD ID (hex) | `i` | string | e.g. `"7706"` |
| HA domain | `d` | string | `sensor`, `binary_sensor`, `switch`, `select`, `number`, `button` |
| Entity name | `n` | string | Human-readable label shown in HA |
| Role | `r` | string | `"s"` = status (read-only), `"r"` = request (write + read paired status) |
| Paired ERD | `p` | string | Paired status ERD hex ID (for request-role entities) |
| Field ID | `fi` | string | Sub-field slug for multi-field ERDs (appended to `unique_id`) |
| Value template | `vt` | string | Jinja2 `value_template` |
| Command template | `ct` | string | Jinja2 `command_template` |
| Options | `o` | JSON array | Select entity option list |
| Unit | `u` | string | `unit_of_measurement` |
| Device class | `dc` | string | HA device class |
| State class | `sc` | string | HA state class |

Example lines:

```jsonl
{"i":"7706","d":"sensor","n":"User Heating Setpoint Status","r":"s","vt":"{{ (value | int(base=16)) / 10 | round(1) }}","u":"°F","dc":"temperature","sc":"measurement"}
{"i":"7707","d":"number","n":"User Heating Setpoint Request","r":"r","p":"7706","vt":"{{ (value | int(base=16)) / 10 | round(1) }}","ct":"{{ '%04x' % ((value | float) * 10 | int) }}","u":"°F","dc":"temperature"}
```

---

## 8. Entity Publishing Pipeline

```
ha_fetch task             Main loop()
──────────────────        ──────────────────────────────────────────────────────
HaDiscoveryItem* ──►  ha_discovery_queue_ (FreeRTOS queue, depth 20)
                              │
                              │  Once per HA_ENTITY_PUBLISH_INTERVAL_MS (50 ms):
                              ▼
                        xQueueReceive → get HaDiscoveryItem*
                              │
                              ▼
                        mqtt_client.publish(topic, payload, retain=true, qos=0)
                              │
                              ▼
                        delete HaDiscoveryItem*
                              │
                              ▼
                        if item == nullptr (sentinel):
                              ha_discovery_publish_in_progress_ = false
                              ha_discovery_published_           = true
```

**Why QoS 0?** QoS 1 causes the broker to send a PUBACK per entity. With ~100 entities published, this generated ~100 inbound MQTT events that could overflow the IDF event queue (`Dropped N inbound MQTT events` warnings). Since topics use `retain=true`, QoS 0 is sufficient.

**Why 50 ms?** `HA_ENTITY_PUBLISH_INTERVAL_MS = 50` gives the MQTT stack time to serialize and transmit one packet before the next is enqueued.

---

## 9. ERD Filtering

`register_erd()` (called by `mqtt_bridge` / `mqtt_bridge_polling` for each ERD the appliance supports) inserts the ERD ID into `ha_registered_erds_` (a `std::set<uint16_t>`).

Before publishing an entity, `process_jsonl_line_()` checks:

```cpp
bool registered = ha_registered_erds_.count(erd_id) > 0;
// For request-role entities, also accept if the paired status ERD is registered
if (!registered && role == 'r' && paired != "") {
    registered |= ha_registered_erds_.count(paired_id) > 0;
}
if (!registered) return false;  // skip
```

If `ha_registered_erds_` is empty (e.g., subscription mode where registration callback was never called), all entities are published as a safety fallback.

---

## 10. Multi-Field ERDs

Some ERDs encode multiple independent values in a single byte array. The generator (`scripts/generate_erd_lists.py`) detects three patterns and emits one line per sub-field:

| Pattern | Example | Result |
|---------|---------|--------|
| **Byte-offset** | MUA CFM Status `0x7464` — 8 bytes, 4 fields (2B each) | One `sensor` entity per field with `value[n:m]` slice in `value_template` |
| **Bitfield** | Relay Status `0x7010` — 1 byte, 8 flag bits | One `binary_sensor` per non-reserved bit with `(value \| int(base=16)) >> n & 1` template |
| **Mixed** | Fan Config Cooling `0x7450` — primary enum + "Allowed Selections" bitmask | Primary `sensor` or `select` + one `binary_sensor` per allowed-selection bit |

Reserved/padding fields are skipped. Each sub-field entity gets a `field_id` (`fi`) appended to its `unique_id` and discovery topic to avoid collisions.

---

## 11. Configuration Parameters

| YAML key | Default | Description |
|----------|---------|-------------|
| `generate_device_config` | `false` | Enable HA discovery. When `true`, JSONL files are fetched at runtime and discovery payloads are published after ERD enumeration settles. |
| `ha_discovery_base_url` | `https://raw.githubusercontent.com/joshualongenecker/home-assistant-bridge-esphome/main/ha_discovery` | Base URL for JSONL category files. Override to point at a local server or a specific branch/tag for testing. |

> **Note**: The default `ha_discovery_base_url` in `__init__.py` currently points to the `copilot/implement-goal-2-autodiscovery` branch. Update it to `main` once that branch is merged.

Example override:

```yaml
geappliances_bridge:
  id: bridge
  ...
  generate_device_config: true
  ha_discovery_base_url: "https://raw.githubusercontent.com/joshualongenecker/home-assistant-bridge-esphome/main/ha_discovery"
```

---

## 12. Lessons Learned

### Timing: timers are unreliable for polling-mode discovery

The original approach used a 10-second quiet timer from bridge initialization. In polling mode, ERD reads during enumeration phases (`add_common_erds`, `add_energy_erds`, `add_appliance_erds`) can be spaced by multi-second gaps when the appliance is slow to respond and retry cycles fire. A 10 s gap mid-enumeration triggers the timer prematurely. The `polling_list_complete` flag set in `state_polling` entry is the only authoritative signal.

### AUTO mode + no subscription activity = silent premature discovery

In AUTO mode with no subscription responses, `ha_discovery_last_activity_` was initialized at bridge init. The original quiet-window check would fire 10 s later, long before the polling bridge had enumerated any ERDs. The fix: gate the quiet-window on `subscription_activity_detected_`, so it only counts down once at least one subscription publication has been received. If none arrive, the code falls through to the `polling_list_complete` check once the bridge falls back to polling.

### Flash size: static C tables don't scale

Compiling the full ERD table (~12,728 entries) as a C array produced a 3.7 MB header that inflated the firmware to 154% of the ESP32-C3's 1.8 MB flash. The solution is to keep only the 16-entry `ha_string_erd_ids` table in flash and fetch the rest at runtime over HTTPS from compact JSONL files in the repository.

### MQTT flooding: QoS 1 at high publish rates overflows the IDF event queue

Publishing ~100 discovery entities with QoS 1 generated ~100 PUBACK events from the broker. The IDF MQTT event queue (depth limited) dropped them, producing `[W][mqtt.idf:094]: Dropped N inbound MQTT events` every 200 ms. Switching to QoS 0 eliminated the inbound events entirely (retained delivery guarantees the broker stores the message; no PUBACK needed).

### FreeRTOS task priority matters for MQTT responsiveness

The `ha_fetch` task was originally spawned at priority 5 (same as the IDF MQTT task). When parsing JSONL and filling the queue, it starved the MQTT task, preventing timely ACK processing. Lowering its priority to 1 allows the MQTT task to preempt it freely.

### One entity per `loop()` iteration is enough — rate-limit at 50 ms, not every loop

`loop()` runs hundreds of times per second. Publishing one entity per call was still too fast for the MQTT stack to drain. Adding a 50 ms interval between entity publishes (`HA_ENTITY_PUBLISH_INTERVAL_MS`) prevents back-to-back fills without meaningfully delaying discovery (100 entities × 50 ms = 5 s total).

### OOM panics from large JSON strings

The original implementation built a single JSON string containing all entity configs in one `loop()` call. At ~94 KB, this allocation failed on a heap that was already fragmented by ESPHome's runtime, causing a watchdog reset. The fix: never accumulate more than one entity's payload in memory at a time.

---

## 13. Known Limitations

- **ESP-IDF only**: The entire HA discovery feature is wrapped in `#ifdef USE_ESP_IDF`. On Arduino targets, `generate_device_config` has no effect.
- **Pending branch merge**: The default `ha_discovery_base_url` in `__init__.py` still points to the development branch. Update to `main` before production use, or override `ha_discovery_base_url` in YAML.
- **One-shot**: Discovery is published once when `publish_ha_discovery_()` fires. If the appliance reconnects and registers different ERDs, discovery is not re-published (the `ha_discovery_published_` guard prevents re-entry). This is intentional to avoid spamming HA on reconnects but may miss dynamic ERD changes.
- **No entity removal**: Retained MQTT discovery messages persist in HA until explicitly deleted. If the set of registered ERDs shrinks (e.g., after a firmware update on the appliance), stale entities remain in HA.
