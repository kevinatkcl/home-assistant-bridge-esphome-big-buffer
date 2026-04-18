"""Tests for generate_erd_lists.py – focusing on signed/unsigned integer handling.

Run with:
    python3 -m pytest scripts/test_generate_erd_lists.py -v
  or:
    python3 -m unittest scripts.test_generate_erd_lists -v
"""

import json
import sys
import unittest
from pathlib import Path

# Make scripts/ importable
sys.path.insert(0, str(Path(__file__).parent))

from jinja2 import Environment

from generate_erd_lists import (
    _is_signed_type,
    _get_primary_data_type,
    _compute_sensor_value_template,
    _byte_subfield_value_template,
    _number_command_template,
    _number_min_max,
    _effective_dtype,
    _infer_dtype_from_jsonl_entry,
    backfill_ha_discovery_jsonl_data_type,
    _collect_ha_discovery_entries,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_jinja_env = Environment()


def _render(template_str: str, value: str) -> str:
    """Render a Jinja2 template string with the given ERD hex value."""
    return _jinja_env.from_string(template_str).render(value=value)


def _render_number(template_str: str, value: float) -> str:
    """Render a command_template with a numeric value (as Home Assistant would)."""
    return _jinja_env.from_string(template_str).render(value=value)


# ---------------------------------------------------------------------------
# _is_signed_type
# ---------------------------------------------------------------------------

class TestIsSignedType(unittest.TestCase):
    def test_signed_types_detected(self):
        for t in ('i8', 'i16', 'i32', 'i64'):
            with self.subTest(t=t):
                self.assertTrue(_is_signed_type(t))

    def test_unsigned_types_not_signed(self):
        for t in ('u8', 'u16', 'u32', 'u64', 'enum', 'bool', 'string', ''):
            with self.subTest(t=t):
                self.assertFalse(_is_signed_type(t))


# ---------------------------------------------------------------------------
# _get_primary_data_type
# ---------------------------------------------------------------------------

class TestGetPrimaryDataType(unittest.TestCase):
    def test_returns_type_of_first_non_reserved_non_bitfield(self):
        data = [{'name': 'Temperature', 'type': 'i16', 'offset': 0, 'size': 2}]
        self.assertEqual(_get_primary_data_type(data), 'i16')

    def test_skips_reserved_fields(self):
        data = [
            {'name': 'Reserved', 'type': 'u8', 'offset': 0, 'size': 1},
            {'name': 'Temperature', 'type': 'i16', 'offset': 1, 'size': 2},
        ]
        self.assertEqual(_get_primary_data_type(data), 'i16')

    def test_skips_bitfield_fields(self):
        data = [
            {'name': 'Flag', 'type': 'u8', 'offset': 0, 'size': 1,
             'bits': {'offset': 0, 'size': 1}},
            {'name': 'Temperature', 'type': 'i16', 'offset': 1, 'size': 2},
        ]
        self.assertEqual(_get_primary_data_type(data), 'i16')

    def test_empty_data_returns_u8(self):
        self.assertEqual(_get_primary_data_type([]), 'u8')

    def test_only_bitfields_returns_u8(self):
        data = [{'name': 'Flag', 'type': 'u8', 'offset': 0, 'size': 1,
                 'bits': {'offset': 0, 'size': 1}}]
        self.assertEqual(_get_primary_data_type(data), 'u8')


# ---------------------------------------------------------------------------
# _compute_sensor_value_template – template string shape
# ---------------------------------------------------------------------------

class TestComputeSensorValueTemplateString(unittest.TestCase):
    def test_unsigned_no_scaling(self):
        tmpl = _compute_sensor_value_template(1, 2, signed=False)
        self.assertEqual(tmpl, '{{ value | int(base=16) }}')

    def test_unsigned_with_scaling(self):
        tmpl = _compute_sensor_value_template(10, 2, signed=False)
        self.assertIn('/ 10', tmpl)
        self.assertNotIn('65536', tmpl)

    def test_signed_contains_sign_extension_for_i16(self):
        tmpl = _compute_sensor_value_template(1, 2, signed=True)
        # max_val = 65536, half_val = 32768
        self.assertIn('65536', tmpl)
        self.assertIn('32768', tmpl)

    def test_signed_i8_uses_correct_thresholds(self):
        tmpl = _compute_sensor_value_template(1, 1, signed=True)
        self.assertIn('256', tmpl)
        self.assertIn('128', tmpl)

    def test_signed_i32_uses_correct_thresholds(self):
        tmpl = _compute_sensor_value_template(1, 4, signed=True)
        self.assertIn('4294967296', tmpl)   # 2^32
        self.assertIn('2147483648', tmpl)   # 2^31


# ---------------------------------------------------------------------------
# _compute_sensor_value_template – Jinja2 rendering correctness
# ---------------------------------------------------------------------------

class TestComputeSensorValueTemplateRendering(unittest.TestCase):
    """Verify that the generated Jinja2 templates evaluate to the right numbers."""

    # --- unsigned int16 ---
    def test_u16_positive(self):
        tmpl = _compute_sensor_value_template(1, 2, signed=False)
        self.assertEqual(_render(tmpl, '0001'), '1')
        self.assertEqual(_render(tmpl, '7fff'), '32767')
        self.assertEqual(_render(tmpl, 'ffff'), '65535')

    # --- signed int16 ---
    def test_i16_negative_one(self):
        tmpl = _compute_sensor_value_template(1, 2, signed=True)
        self.assertEqual(int(_render(tmpl, 'ffff')), -1)

    def test_i16_negative_two(self):
        tmpl = _compute_sensor_value_template(1, 2, signed=True)
        self.assertEqual(int(_render(tmpl, 'fffe')), -2)

    def test_i16_min_value(self):
        tmpl = _compute_sensor_value_template(1, 2, signed=True)
        self.assertEqual(int(_render(tmpl, '8000')), -32768)

    def test_i16_max_value(self):
        tmpl = _compute_sensor_value_template(1, 2, signed=True)
        self.assertEqual(int(_render(tmpl, '7fff')), 32767)

    def test_i16_zero(self):
        tmpl = _compute_sensor_value_template(1, 2, signed=True)
        self.assertEqual(int(_render(tmpl, '0000')), 0)

    def test_i16_positive_one(self):
        tmpl = _compute_sensor_value_template(1, 2, signed=True)
        self.assertEqual(int(_render(tmpl, '0001')), 1)

    # --- signed int8 ---
    def test_i8_negative_one(self):
        tmpl = _compute_sensor_value_template(1, 1, signed=True)
        self.assertEqual(int(_render(tmpl, 'ff')), -1)

    def test_i8_negative_two(self):
        tmpl = _compute_sensor_value_template(1, 1, signed=True)
        self.assertEqual(int(_render(tmpl, 'fe')), -2)

    def test_i8_min_value(self):
        tmpl = _compute_sensor_value_template(1, 1, signed=True)
        self.assertEqual(int(_render(tmpl, '80')), -128)

    def test_i8_max_value(self):
        tmpl = _compute_sensor_value_template(1, 1, signed=True)
        self.assertEqual(int(_render(tmpl, '7f')), 127)

    # --- signed int32 ---
    def test_i32_negative_one(self):
        tmpl = _compute_sensor_value_template(1, 4, signed=True)
        self.assertEqual(int(_render(tmpl, 'ffffffff')), -1)

    def test_i32_min_value(self):
        tmpl = _compute_sensor_value_template(1, 4, signed=True)
        self.assertEqual(int(_render(tmpl, '80000000')), -2147483648)

    def test_i32_max_value(self):
        tmpl = _compute_sensor_value_template(1, 4, signed=True)
        self.assertEqual(int(_render(tmpl, '7fffffff')), 2147483647)

    # --- signed with scaling ---
    def test_i16_scaling_10_negative(self):
        tmpl = _compute_sensor_value_template(10, 2, signed=True)
        # 0xFFF6 = 65526 unsigned = -10 signed → -10 / 10 = -1.0
        result = float(_render(tmpl, 'fff6'))
        self.assertAlmostEqual(result, -1.0, places=1)

    def test_i16_scaling_10_positive(self):
        tmpl = _compute_sensor_value_template(10, 2, signed=True)
        # 0x0064 = 100 → 100 / 10 = 10.0
        result = float(_render(tmpl, '0064'))
        self.assertAlmostEqual(result, 10.0, places=1)


# ---------------------------------------------------------------------------
# _byte_subfield_value_template – Jinja2 rendering correctness
# ---------------------------------------------------------------------------

class TestByteSubfieldValueTemplateRendering(unittest.TestCase):
    """Verify signed sub-field extraction from multi-byte ERD payloads."""

    def _make_i16_field(self, offset: int) -> dict:
        return {'name': 'Temp', 'type': 'i16', 'offset': offset, 'size': 2}

    def _make_i8_field(self, offset: int) -> dict:
        return {'name': 'Temp', 'type': 'i8', 'offset': offset, 'size': 1}

    def _make_u16_field(self, offset: int) -> dict:
        return {'name': 'Val', 'type': 'u16', 'offset': offset, 'size': 2}

    # --- unsigned (should not change existing behaviour) ---
    def test_u16_at_offset0(self):
        tmpl = _byte_subfield_value_template(self._make_u16_field(0), 1)
        self.assertEqual(_render(tmpl, 'ffff'), '65535')

    def test_u16_positive(self):
        tmpl = _byte_subfield_value_template(self._make_u16_field(0), 1)
        self.assertEqual(_render(tmpl, '0064'), '100')

    # --- signed i16 at offset 0 ---
    def test_i16_at_offset0_negative_one(self):
        tmpl = _byte_subfield_value_template(self._make_i16_field(0), 1)
        self.assertEqual(int(_render(tmpl, 'ffff')), -1)

    def test_i16_at_offset0_negative_two(self):
        tmpl = _byte_subfield_value_template(self._make_i16_field(0), 1)
        self.assertEqual(int(_render(tmpl, 'fffe')), -2)

    def test_i16_at_offset0_min(self):
        tmpl = _byte_subfield_value_template(self._make_i16_field(0), 1)
        self.assertEqual(int(_render(tmpl, '8000')), -32768)

    def test_i16_at_offset0_max(self):
        tmpl = _byte_subfield_value_template(self._make_i16_field(0), 1)
        self.assertEqual(int(_render(tmpl, '7fff')), 32767)

    def test_i16_at_offset0_zero(self):
        tmpl = _byte_subfield_value_template(self._make_i16_field(0), 1)
        self.assertEqual(int(_render(tmpl, '0000')), 0)

    # --- signed i16 at offset 2 (second field in a multi-field ERD) ---
    def test_i16_at_offset2_negative_one(self):
        tmpl = _byte_subfield_value_template(self._make_i16_field(2), 1)
        # First 2 bytes are something else; last 2 are 0xFFFF
        self.assertEqual(int(_render(tmpl, '0000ffff')), -1)

    def test_i16_at_offset2_min(self):
        tmpl = _byte_subfield_value_template(self._make_i16_field(2), 1)
        self.assertEqual(int(_render(tmpl, '00008000')), -32768)

    def test_i16_at_offset2_positive(self):
        tmpl = _byte_subfield_value_template(self._make_i16_field(2), 1)
        self.assertEqual(int(_render(tmpl, '00007fff')), 32767)

    # --- signed i8 ---
    def test_i8_at_offset0_negative_one(self):
        tmpl = _byte_subfield_value_template(self._make_i8_field(0), 1)
        self.assertEqual(int(_render(tmpl, 'ff')), -1)

    def test_i8_at_offset0_min(self):
        tmpl = _byte_subfield_value_template(self._make_i8_field(0), 1)
        self.assertEqual(int(_render(tmpl, '80')), -128)

    def test_i8_at_offset1_negative_one(self):
        tmpl = _byte_subfield_value_template(self._make_i8_field(1), 1)
        self.assertEqual(int(_render(tmpl, '00ff')), -1)

    # --- signed i16 with scaling ---
    def test_i16_at_offset0_scale10_negative(self):
        tmpl = _byte_subfield_value_template(self._make_i16_field(0), 10)
        # 0xFFF6 = -10 → -10/10 = -1.0
        result = float(_render(tmpl, 'fff6'))
        self.assertAlmostEqual(result, -1.0, places=1)


# ---------------------------------------------------------------------------
# _number_command_template – Jinja2 rendering correctness
# ---------------------------------------------------------------------------

class TestNumberCommandTemplateRendering(unittest.TestCase):
    """Verify that command templates encode negative values as two's-complement hex."""

    # --- unsigned ---
    def test_u16_positive(self):
        tmpl = _number_command_template(2, 1, signed=False)
        self.assertEqual(_render_number(tmpl, 100), '0064')

    def test_u16_zero(self):
        tmpl = _number_command_template(2, 1, signed=False)
        self.assertEqual(_render_number(tmpl, 0), '0000')

    # --- signed int16: negative values must wrap to two's-complement hex ---
    def test_i16_negative_one(self):
        tmpl = _number_command_template(2, 1, signed=True)
        self.assertEqual(_render_number(tmpl, -1), 'ffff')

    def test_i16_negative_two(self):
        tmpl = _number_command_template(2, 1, signed=True)
        self.assertEqual(_render_number(tmpl, -2), 'fffe')

    def test_i16_min(self):
        tmpl = _number_command_template(2, 1, signed=True)
        self.assertEqual(_render_number(tmpl, -32768), '8000')

    def test_i16_max(self):
        tmpl = _number_command_template(2, 1, signed=True)
        self.assertEqual(_render_number(tmpl, 32767), '7fff')

    def test_i16_zero(self):
        tmpl = _number_command_template(2, 1, signed=True)
        self.assertEqual(_render_number(tmpl, 0), '0000')

    # --- signed int8 ---
    def test_i8_negative_one(self):
        tmpl = _number_command_template(1, 1, signed=True)
        self.assertEqual(_render_number(tmpl, -1), 'ff')

    def test_i8_min(self):
        tmpl = _number_command_template(1, 1, signed=True)
        self.assertEqual(_render_number(tmpl, -128), '80')

    # --- signed with scaling ---
    def test_i16_scaling_negative(self):
        tmpl = _number_command_template(2, 10, signed=True)
        # User inputs -1.0; scaled: -1.0 * 10 = -10 → 0xFFF6
        self.assertEqual(_render_number(tmpl, -1.0), 'fff6')


# ---------------------------------------------------------------------------
# _collect_ha_discovery_entries – end-to-end value_template checks
# ---------------------------------------------------------------------------

class TestCollectHaDiscoveryEntriesSigned(unittest.TestCase):
    """Verify that _collect_ha_discovery_entries produces correct templates
    for signed-integer ERDs and that unsigned ERDs are unaffected."""

    def _entry_for(self, erds, name):
        entries = _collect_ha_discovery_entries(erds)
        return next((e for e in entries if e['name'] == name), None)

    def test_single_field_i16_sensor_template(self):
        erds = [{
            'id': '0x1000', 'name': 'Temperature', 'ha_domain': 'sensor',
            'device_class': 'temperature', 'state_class': 'measurement',
            'data': [{'name': 'Temperature', 'type': 'i16', 'offset': 0, 'size': 2}],
        }]
        e = self._entry_for(erds, 'Temperature')
        self.assertIsNotNone(e)
        tmpl = e['value_template']
        self.assertEqual(int(_render(tmpl, 'ffff')), -1)
        self.assertEqual(int(_render(tmpl, 'fffe')), -2)
        self.assertEqual(int(_render(tmpl, '8000')), -32768)
        self.assertEqual(int(_render(tmpl, '7fff')), 32767)
        self.assertEqual(int(_render(tmpl, '0000')), 0)

    def test_single_field_u16_sensor_template_unchanged(self):
        erds = [{
            'id': '0x1001', 'name': 'Voltage', 'ha_domain': 'sensor',
            'device_class': 'voltage', 'state_class': 'measurement',
            'data': [{'name': 'Voltage', 'type': 'u16', 'offset': 0, 'size': 2}],
        }]
        e = self._entry_for(erds, 'Voltage')
        self.assertIsNotNone(e)
        tmpl = e['value_template']
        self.assertEqual(_render(tmpl, 'ffff'), '65535')
        self.assertEqual(_render(tmpl, '0064'), '100')

    def test_multi_field_i16_byte_offset_each_field(self):
        erds = [{
            'id': '0x1002', 'name': 'Display Temp',
            'ha_domain': 'sensor', 'device_class': 'temperature',
            'data': [
                {'name': 'Fresh Food Temp', 'type': 'i16', 'offset': 0, 'size': 2},
                {'name': 'Freezer Temp',    'type': 'i16', 'offset': 2, 'size': 2},
            ],
        }]
        entries = _collect_ha_discovery_entries(erds)
        self.assertEqual(len(entries), 2)
        fresh = next(e for e in entries if 'Fresh Food' in e['name'])
        freezer = next(e for e in entries if 'Freezer' in e['name'])

        # Fresh Food at offset 0
        self.assertEqual(int(_render(fresh['value_template'], 'ffff0000')), -1)
        self.assertEqual(int(_render(fresh['value_template'], '80000000')), -32768)
        self.assertEqual(int(_render(fresh['value_template'], '7fff0000')), 32767)

        # Freezer at offset 2
        self.assertEqual(int(_render(freezer['value_template'], '0000ffff')), -1)
        self.assertEqual(int(_render(freezer['value_template'], '00008000')), -32768)
        self.assertEqual(int(_render(freezer['value_template'], '00007fff')), 32767)

    def test_mixed_signed_unsigned_sub_fields(self):
        erds = [{
            'id': '0x1003', 'name': 'Mixed', 'ha_domain': 'sensor',
            'data': [
                {'name': 'Signed Val',   'type': 'i8',  'offset': 0, 'size': 1},
                {'name': 'Unsigned Val', 'type': 'u8',  'offset': 1, 'size': 1},
            ],
        }]
        entries = _collect_ha_discovery_entries(erds)
        self.assertEqual(len(entries), 2)
        signed_e   = next(e for e in entries if 'Signed' in e['name'])
        unsigned_e = next(e for e in entries if 'Unsigned' in e['name'])

        # Signed i8 at offset 0
        self.assertEqual(int(_render(signed_e['value_template'], 'ff00')), -1)
        self.assertEqual(int(_render(signed_e['value_template'], '8000')), -128)
        self.assertEqual(int(_render(signed_e['value_template'], '7f00')), 127)

        # Unsigned u8 at offset 1: should NOT be sign-extended
        self.assertEqual(_render(unsigned_e['value_template'], '00ff'), '255')

    def test_i16_sensor_with_scaling(self):
        erds = [{
            'id': '0x1004', 'name': 'Offset Temp', 'ha_domain': 'sensor',
            'device_class': 'temperature', 'scaling_factor': 10,
            'data': [{'name': 'Offset Temp', 'type': 'i16', 'offset': 0, 'size': 2}],
        }]
        e = self._entry_for(erds, 'Offset Temp')
        tmpl = e['value_template']
        # 0xFFF6 = -10 signed → -10 / 10 = -1.0
        self.assertAlmostEqual(float(_render(tmpl, 'fff6')), -1.0, places=1)
        # 0x0064 = 100 → 100 / 10 = 10.0
        self.assertAlmostEqual(float(_render(tmpl, '0064')), 10.0, places=1)

    def test_i16_number_entity_command_template_roundtrip(self):
        """A number entity backed by a signed int16 must round-trip negative values."""
        erds = [{
            'id': '0x1005', 'name': 'Set Temp', 'ha_domain': 'number',
            'device_class': 'temperature',
            'data': [{'name': 'Set Temp', 'type': 'i16', 'offset': 0, 'size': 2}],
        }]
        e = self._entry_for(erds, 'Set Temp')
        vt = e['value_template']
        ct = e['command_template']

        # value_template: 0xFFFF → -1
        self.assertEqual(int(_render(vt, 'ffff')), -1)
        # command_template: -1 → 'ffff'
        self.assertEqual(_render_number(ct, -1), 'ffff')

        # command_template: -32768 → '8000'
        self.assertEqual(_render_number(ct, -32768), '8000')
        # command_template: 32767 → '7fff'
        self.assertEqual(_render_number(ct, 32767), '7fff')

        # Number entity must carry a data_type so the C++ runtime can derive
        # the correct min/max bounds (not the HA default 0-100 range).
        self.assertEqual(e['data_type'], 'int16')


class TestNumberMinMax(unittest.TestCase):
    """Verify _number_min_max returns correct ranges and step values."""

    def test_unsigned_1byte(self):
        mn, mx, st = _number_min_max(1, 1, False)
        self.assertEqual(mn, 0.0)
        self.assertEqual(mx, 255.0)
        self.assertEqual(st, 1.0)

    def test_unsigned_2byte(self):
        mn, mx, st = _number_min_max(2, 1, False)
        self.assertEqual(mn, 0.0)
        self.assertEqual(mx, 65535.0)
        self.assertEqual(st, 1.0)

    def test_unsigned_2byte_scale10(self):
        mn, mx, st = _number_min_max(2, 10, False)
        self.assertAlmostEqual(mn, 0.0)
        self.assertAlmostEqual(mx, 6553.5)
        self.assertAlmostEqual(st, 0.1)

    def test_signed_1byte(self):
        mn, mx, st = _number_min_max(1, 1, True)
        self.assertEqual(mn, -128.0)
        self.assertEqual(mx, 127.0)
        self.assertEqual(st, 1.0)

    def test_signed_2byte(self):
        mn, mx, st = _number_min_max(2, 1, True)
        self.assertEqual(mn, -32768.0)
        self.assertEqual(mx, 32767.0)

    def test_large_effective_bytes_capped_at_4(self):
        # ds=20 should be capped at 4 bytes (uint32 max)
        mn, mx, _ = _number_min_max(20, 1, False)
        self.assertEqual(mx, 4294967295.0)

    def test_step_equals_1_when_no_scaling(self):
        _, _, st = _number_min_max(2, 1, False)
        self.assertEqual(st, 1.0)

    def test_step_equals_one_tenth_when_scale10(self):
        _, _, st = _number_min_max(2, 10, False)
        self.assertAlmostEqual(st, 0.1)


class TestEffectiveDtype(unittest.TestCase):
    """Verify _effective_dtype maps byte counts and signedness to type strings."""

    def test_unsigned_1byte(self):
        self.assertEqual(_effective_dtype(1, False), 'uint8')

    def test_unsigned_2byte(self):
        self.assertEqual(_effective_dtype(2, False), 'uint16')

    def test_unsigned_3byte(self):
        self.assertEqual(_effective_dtype(3, False), 'uint24')

    def test_unsigned_4byte(self):
        self.assertEqual(_effective_dtype(4, False), 'uint32')

    def test_unsigned_large_capped_to_uint32(self):
        self.assertEqual(_effective_dtype(8, False), 'uint32')

    def test_signed_1byte(self):
        self.assertEqual(_effective_dtype(1, True), 'int8')

    def test_signed_2byte(self):
        self.assertEqual(_effective_dtype(2, True), 'int16')

    def test_signed_4byte(self):
        self.assertEqual(_effective_dtype(4, True), 'int32')

    def test_signed_large_capped_to_int32(self):
        self.assertEqual(_effective_dtype(8, True), 'int32')


class TestInferDtypeFromJsonlEntry(unittest.TestCase):
    """Verify _infer_dtype_from_jsonl_entry reads ds/vt to produce the right type string."""

    def test_unsigned_1byte(self):
        self.assertEqual(_infer_dtype_from_jsonl_entry({'ds': 1, 'vt': '{{ value | int(base=16) }}'}), 'uint8')

    def test_unsigned_2byte(self):
        self.assertEqual(_infer_dtype_from_jsonl_entry({'ds': 2, 'vt': '{{ value | int(base=16) }}'}), 'uint16')

    def test_unsigned_2byte_with_scale(self):
        vt = '{{ (value | int(base=16)) / 10 | round(1) }}'
        self.assertEqual(_infer_dtype_from_jsonl_entry({'ds': 2, 'sf': 10, 'vt': vt}), 'uint16')

    def test_slice_notation_limits_effective_bytes(self):
        # value[0:4] = 2 bytes effective, even though ds=4
        vt = '{{ value[0:4] | int(base=16) }}'
        self.assertEqual(_infer_dtype_from_jsonl_entry({'ds': 4, 'vt': vt}), 'uint16')

    def test_signed_vt_detected(self):
        vt = '{{ (value | int(base=16)) - 65536 if (value | int(base=16)) >= 32768 else (value | int(base=16)) }}'
        self.assertEqual(_infer_dtype_from_jsonl_entry({'ds': 2, 'vt': vt}), 'int16')

    def test_signed_1byte(self):
        vt = '{{ (value | int(base=16)) - 256 if (value | int(base=16)) >= 128 else (value | int(base=16)) }}'
        self.assertEqual(_infer_dtype_from_jsonl_entry({'ds': 1, 'vt': vt}), 'int8')

    def test_missing_ds_defaults_to_uint8(self):
        self.assertEqual(_infer_dtype_from_jsonl_entry({'vt': '{{ value | int(base=16) }}'}), 'uint8')

    def test_3byte_yields_uint24(self):
        self.assertEqual(_infer_dtype_from_jsonl_entry({'ds': 3, 'vt': '{{ value | int(base=16) }}'}), 'uint24')

    def test_large_ds_capped_to_uint32(self):
        self.assertEqual(_infer_dtype_from_jsonl_entry({'ds': 8, 'vt': '{{ value | int(base=16) }}'}), 'uint32')


class TestBackfillHaDiscoveryJsonlDataType(unittest.TestCase):
    """Verify backfill_ha_discovery_jsonl_data_type updates JSONL files correctly."""

    def setUp(self):
        import tempfile
        self.tmpdir = Path(tempfile.mkdtemp())

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmpdir)

    def _write_jsonl(self, name: str, lines: list) -> Path:
        p = self.tmpdir / name
        p.write_text('\n'.join(json.dumps(obj, separators=(',', ':')) for obj in lines) + '\n')
        return p

    def _read_jsonl(self, path: Path) -> list:
        return [json.loads(l) for l in path.read_text().splitlines() if l.strip()]

    def test_adds_dt_to_number_entities(self):
        p = self._write_jsonl('test.jsonl', [
            {'i': '1234', 'n': 'Widget', 'd': 'number', 'ds': 1, 'vt': '{{ value | int(base=16) }}', 'ct': '{{ \'%02x\' % (value | int) }}'},
            {'i': '5678', 'n': 'Sensor', 'd': 'sensor', 'ds': 2},
        ])
        n = backfill_ha_discovery_jsonl_data_type(self.tmpdir)
        self.assertEqual(n, 1)
        entries = self._read_jsonl(p)
        number_e = next(e for e in entries if e['d'] == 'number')
        self.assertEqual(number_e['dt'], 'uint8')
        # mn/mx/st must not be present
        self.assertNotIn('mn', number_e)
        self.assertNotIn('mx', number_e)
        self.assertNotIn('st', number_e)

    def test_removes_old_mn_mx_st_fields(self):
        p = self._write_jsonl('test.jsonl', [
            {'i': '1234', 'n': 'Widget', 'd': 'number', 'ds': 1,
             'mn': 0.0, 'mx': 255.0,
             'vt': '{{ value | int(base=16) }}', 'ct': '{{ \'%02x\' % (value | int) }}'},
        ])
        backfill_ha_discovery_jsonl_data_type(self.tmpdir)
        entries = self._read_jsonl(p)
        self.assertEqual(entries[0].get('dt'), 'uint8')
        self.assertNotIn('mn', entries[0])
        self.assertNotIn('mx', entries[0])

    def test_does_not_overwrite_existing_dt(self):
        p = self._write_jsonl('test.jsonl', [
            {'i': '1234', 'n': 'Widget', 'd': 'number', 'ds': 1, 'dt': 'int8',
             'vt': '{{ value | int(base=16) }}', 'ct': '{{ \'%02x\' % (value | int) }}'},
        ])
        n = backfill_ha_discovery_jsonl_data_type(self.tmpdir)
        self.assertEqual(n, 0)
        entries = self._read_jsonl(p)
        self.assertEqual(entries[0]['dt'], 'int8')  # unchanged

    def test_uint16_with_scale(self):
        p = self._write_jsonl('test.jsonl', [
            {'i': 'abcd', 'n': 'Temp', 'd': 'number', 'ds': 2, 'sf': 10,
             'vt': '{{ (value | int(base=16)) / 10 | round(1) }}', 'ct': '{{ \'%04x\' % ((value | float) * 10 | int) }}'},
        ])
        backfill_ha_discovery_jsonl_data_type(self.tmpdir)
        entries = self._read_jsonl(p)
        self.assertEqual(entries[0]['dt'], 'uint16')

    def test_non_number_entities_unchanged(self):
        p = self._write_jsonl('test.jsonl', [
            {'i': '0001', 'n': 'Switch', 'd': 'switch', 'ds': 1},
            {'i': '0002', 'n': 'Sensor', 'd': 'sensor', 'ds': 2},
        ])
        n = backfill_ha_discovery_jsonl_data_type(self.tmpdir)
        self.assertEqual(n, 0)
        entries = self._read_jsonl(p)
        for e in entries:
            self.assertNotIn('dt', e)


if __name__ == '__main__':
    unittest.main()
