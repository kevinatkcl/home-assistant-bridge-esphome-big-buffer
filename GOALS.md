# Project Goals and Proposed Improvements

## Vision

This repository enables communication between GE Appliances and Home Assistant using the GEA3 protocol over an ESPHome-based hardware adapter. The long-term goal is to provide a seamless, zero-configuration experience: plug the adapter into an appliance, enter WiFi credentials once, and have the appliance's sensors and controls automatically appear in Home Assistant as first-class entities — no YAML editing, no ERD knowledge required.

The following improvements are proposed to move closer to that vision.

---

## Proposed Improvements

### 1. Zero-Configuration WiFi Provisioning

**Goal:** Users should not need to modify YAML configuration files to enter WiFi credentials.

**How it could be achieved:**
- Use ESPHome's built-in [Improv via BLE](https://esphome.io/components/esp32_improv.html) component so that a user can send WiFi credentials from the Home Assistant mobile app or web browser via Bluetooth, with no prior configuration.
- As a fallback, enable ESPHome's [Captive Portal](https://esphome.io/components/captive_portal.html) component, which broadcasts a WiFi access point on first boot. The user connects to that AP and enters their credentials in a web form.
- Together these two features mean the only pre-flashed configuration that changes between users is the firmware itself — no `secrets.yaml` or YAML editing needed.

---

### 2. Automatic Home Assistant Entity Creation via MQTT Discovery

**Goal:** When the adapter connects to an appliance, entities (sensors, switches, selects, etc.) should automatically appear in Home Assistant without any manual configuration.

**How it could be achieved:**
- Leverage Home Assistant's [MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery) protocol. After the appliance type and available ERDs are identified, publish discovery payloads to `homeassistant/<domain>/<device_id>/<object_id>/config` for each ERD.
- Map known ERD IDs from the [GE Appliances Public API Documentation](https://github.com/geappliances/public-appliance-api-documentation) to their Home Assistant entity domain (e.g., `sensor`, `binary_sensor`, `switch`, `select`, `number`) using the ERD metadata already present in the JSON definitions.
- Include friendly names, units of measurement, device class, and state class in the discovery payload so entities are immediately useful (e.g., a refrigerator temperature sensor would appear with `°F`, device class `temperature`, and state class `measurement`).
- Only publish discovery messages for ERDs that are actually readable on the connected appliance, keeping the entity list clean and relevant.

---

### 3. Decoded Human-Readable Entity Values

**Goal:** Entity values in Home Assistant should be meaningful (e.g., `38 °F` or `Running`) rather than raw hex strings (e.g., `0026`).

**How it could be achieved:**
- Use the ERD definitions in `appliance_api_erd_definitions.json` to understand each ERD's data type, byte layout, and enumeration mappings.
- Implement a value decoder in the bridge component that transforms the raw binary ERD payload into an appropriate type before publishing to MQTT:
  - Integer ERDs → publish as a numeric string with the correct unit.
  - Enumeration ERDs → publish the string label (e.g., `Idle`, `Washing`, `Rinsing`).
  - Boolean/bitfield ERDs → publish `ON`/`OFF` or individual binary sensor values per bit.
  - String ERDs (model/serial) → publish as-is.
- This decoding can be driven by a generated C++ lookup table produced by extending `generate_erd_lists.py` with type and scaling metadata from the API documentation JSON.

---

### 4. Writable Entity Support for Appliance Control

**Goal:** Users should be able to control the appliance from the Home Assistant UI (e.g., set target temperature, change wash cycle, toggle a setting) without knowing ERD IDs.

**How it could be achieved:**
- During MQTT Discovery, classify writable ERDs and publish them as controllable entity types (`switch`, `select`, `number`, `climate`) in addition to sensors.
- For each writable ERD, subscribe to the corresponding Home Assistant command topic. When a command arrives, encode the value back into the ERD binary format and write it to the appliance via GEA3.
- Example: A `climate` entity for a refrigerator could map to the setpoint ERD, allowing the user to adjust the target temperature directly from the HA thermostat card.
- The existing `geappliances/<device_id>/erd/<erd_id>/write` MQTT path already handles the transport; the improvement is in auto-generating the discovery config and value encoding/decoding layer above it.

---

### 5. Appliance Availability Tracking

**Goal:** Home Assistant should accurately reflect whether the appliance is reachable or not.

**How it could be achieved:**
- Publish an availability payload (e.g., `online`/`offline`) to an MQTT availability topic (`geappliances/<device_id>/status`) using ESPHome's `will_message` (last-will-and-testament) feature so Home Assistant marks the device unavailable if the adapter disconnects.
- Additionally, track GEA3 bus activity: if no ERD responses are received for a configurable timeout period, publish `offline` to the availability topic and trigger a re-discovery cycle.
- Include the `availability_topic` field in every MQTT Discovery config payload so all entities in the device reflect its current connectivity status.

---

### 6. Native Home Assistant API Support (No MQTT Broker Required)

**Goal:** Allow the adapter to integrate with Home Assistant directly, without requiring a separate MQTT broker to be configured.

**How it could be achieved:**
- ESPHome provides a [Native API](https://esphome.io/components/api.html) that communicates directly with the Home Assistant instance over the local network, with no MQTT broker required.
- In this model, ERD values would be exposed as ESPHome `sensor`, `binary_sensor`, `switch`, `select`, and `number` components declared dynamically at runtime via custom ESPHome components.
- The main challenge is that ESPHome's native API entity list is static at compile time, but a dynamic approach is possible through the use of ESPHome's custom component framework and the `homeassistant.tag_scanned` / entity registration patterns, or by generating the entity list at build time from the appliance API documentation for each known appliance type.
- An intermediate step could be to ship a pre-compiled firmware variant per appliance type (e.g., `dishwasher.bin`, `refrigerator.bin`) with the full entity list baked in.

---

### 7. Per-Appliance-Type Firmware Variants

**Goal:** Reduce the number of ERDs polled or subscribed to only those relevant to the connected appliance.

**How it could be achieved:**
- The `generate_erd_lists.py` script already partitions ERDs by appliance type (refrigeration, laundry, dishwasher, etc.). Extend this to generate a separate set of ERD lists per appliance type.
- At build time (or via ESPHome's `substitutions` mechanism), select the correct ERD list based on a user-specified or auto-detected appliance type.
- This reduces firmware size, speeds up polling cycles, and results in a cleaner entity list in Home Assistant.
- Paired with improvement #6, this approach enables generating a complete, correctly-typed entity list at compile time rather than needing dynamic runtime discovery.

---

### 8. Over-the-Air (OTA) Firmware Updates

**Goal:** Users should be able to receive firmware updates without needing physical access to the adapter or knowing how to use ESPHome.

**How it could be achieved:**
- ESPHome's built-in [OTA Update](https://esphome.io/components/ota/esphome.html) component already supports this at the developer level.
- For an end-user experience, integrate with Home Assistant's [ESPHome add-on](https://esphome.io/guides/getting_started_hassio.html), which provides a dashboard for managing and updating ESPHome devices. Users can trigger updates from the HA UI.
- For users not running the ESPHome add-on, the [HTTP OTA](https://esphome.io/components/update/http_request.html) component can periodically check a hosted manifest file for new firmware versions and prompt the user to update via HA or the ESPHome web UI.

---

### 9. Local Web Dashboard for Status and Diagnostics

**Goal:** Provide a local web interface to inspect the adapter's state, view discovered ERDs, and troubleshoot — without needing Home Assistant or MQTT.

**How it could be achieved:**
- ESPHome's built-in [Web Server](https://esphome.io/components/web_server.html) component serves a status page at `http://<device-ip>/` showing all entity states.
- Extend the custom component to publish ERD values as ESPHome internal sensors so they appear on the web server page.
- Add a diagnostics endpoint that lists the discovered appliance address, protocol, device ID, and a log of recent ERD updates. This can be implemented as a custom HTTP handler within the ESPHome framework.
- This makes it possible to verify the adapter is working correctly without any cloud or broker dependency.

---

### 10. Multi-Appliance Support on a Single Adapter

**Goal:** Allow one adapter to bridge multiple appliances on the same GEA3 bus, or support daisy-chained adapters.

**How it could be achieved:**
- The GEA3 protocol is a bus topology: multiple appliances can share the same serial bus. The existing autodiscovery logic already collects all responding board addresses; currently only the first is used.
- Extend the bridge to maintain a per-appliance state machine, ERD subscription/polling list, and MQTT topic namespace (`geappliances/<device_id_1>/`, `geappliances/<device_id_2>/`, etc.) for each discovered appliance.
- Publish a separate set of MQTT Discovery messages for each appliance, so they appear as distinct devices in Home Assistant.
- This is particularly useful for paired appliance suites (e.g., a washer and dryer connected on the same bus).

---

### 11. Persistent ERD Cache Across Reboots

**Goal:** Avoid a slow startup period where entities are unavailable while the adapter re-reads ERD values after a reboot.

**How it could be achieved:**
- Use ESPHome's [Flash storage / globals with restore_value](https://esphome.io/components/globals.html) or [Preferences](https://esphome.io/components/esp32.html) to persist the last-known values of key ERDs (especially slowly-changing ones like model number, serial number, and appliance type) across reboots.
- On startup, immediately publish the cached values to MQTT so Home Assistant entities are populated before the first live ERD read completes.
- Mark cached values with a `cached` quality flag or set the entity state to `unavailable` with the last value retained, depending on Home Assistant's availability semantics.

---

### 12. Expanded ERD Metadata and Documentation

**Goal:** Make it easier for developers and advanced users to understand what each ERD represents and how to interact with it.

**How it could be achieved:**
- Extend `generate_erd_lists.py` to emit a companion JSON or Markdown file that maps each ERD ID to its name, description, data type, unit, access level (read-only / read-write), and known enumeration values, sourced from `appliance_api_erd_definitions.json`.
- Host this generated reference as part of the repository documentation (e.g., `doc/erd-reference.md`) so users can look up what a particular MQTT topic corresponds to without needing to parse the JSON themselves.
- Cross-reference this with the improvement to produce friendly entity names and decoded values (improvements #2 and #3).
