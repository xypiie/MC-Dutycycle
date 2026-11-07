# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Peter Kästle
"""Validate the RF preset against the firmware 1.5.1 loader's wire format."""

from pathlib import Path
import re
import unittest


class RadioPresetTest(unittest.TestCase):
    def registers(self):
        source = (Path(__file__).resolve().parents[1] / "mc_dutycycle.c").read_text()
        body = re.search(r"rssi_preset\[\]\s*=\s*\{(.*?)\};", source, re.S).group(1)
        body = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S)
        data = [int(token.strip(), 0) for token in body.split(",") if token.strip()]
        registers = {}
        offset = 0
        # furi_hal_subghz_load_custom_preset stops at the first zero address,
        # then consumes a two-byte terminator followed by eight PA bytes.
        while offset < len(data) and data[offset] != 0:
            self.assertLess(offset + 1, len(data), "Incomplete register pair")
            self.assertLessEqual(data[offset], 0x2E, "Not a configuration register")
            registers[data[offset]] = data[offset + 1]
            offset += 2
        self.assertEqual(data[offset:offset + 2], [0, 0], "Missing preset terminator")
        self.assertEqual(len(data) - offset, 10, "Preset ends before its RF settings")
        self.assertEqual(data[offset + 2:], [0] * 8, "RX-only preset must have zero PA bytes")
        return registers

    def test_receiver_stays_in_continuous_rssi_mode(self):
        registers = self.registers()
        self.assertEqual(registers[0x12] & 7, 0, "Sync detection would latch RSSI")
        self.assertEqual((registers[0x08] >> 4) & 3, 3, "Use asynchronous serial mode")
        self.assertEqual((registers[0x17] >> 2) & 3, 3, "Stay in RX after reception")

    def test_bandwidth_covers_the_eu_narrow_channel(self):
        config = self.registers()[0x10]
        exponent = config >> 6
        mantissa = (config >> 4) & 3
        bandwidth = 26_000_000 / (8 * (4 + mantissa) * 2 ** exponent)
        self.assertGreaterEqual(bandwidth, 62_500)
        self.assertLessEqual(bandwidth, 125_000)


if __name__ == "__main__":
    unittest.main()
