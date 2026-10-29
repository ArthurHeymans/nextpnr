#!/usr/bin/env python3
"""Synthetic checkpoint/report checker tests; no device timing qualification."""
import copy
import tempfile
import unittest
from pathlib import Path

from timing_reload import assert_preserved, assert_report_reproduced, load, routes


def fixture():
    return {"modules": {"top": {
        "attributes": {"top": "1"}, "settings": {"timing.reanalysis.model": "legacy_chipdb"},
        "ports": {"pad": {"direction": "input", "bits": [10]}},
        "cells": {"ff": {"type": "DFF", "parameters": {"INIT": "0"},
                           "attributes": {"NEXTPNR_BEL": "X1Y1/DFF0", "BEL_STRENGTH": "101"},
                           "port_directions": {"D": "input", "Q": "output"},
                           "connections": {"D": [10], "Q": [11]}}},
        "netnames": {"pad": {"bits": [10], "attributes": {"ROUTING": "sink;pip;5;root;;5"}},
                     "q": {"bits": [11], "attributes": {"ROUTING": " "}},
                     "unused": {"bits": [12], "attributes": {"ROUTING": " ", "src": "kept"}}}
    }}}


def report_fixture():
    return {"fmax": {"clk": {"achieved": 100.0, "constraint": 120.0}},
            "utilization": {"DFF": {"used": 1, "available": 8}},
            "critical_paths": [{"from": "posedge clk", "to": "posedge clk",
                                "path": [{"type": "clk-to-q", "delay": 0.383}]}],
            "detailed_net_timings": [{"driver": "ff", "port": "Q", "event": "posedge clk", "net": "q",
                                      "endpoints": [{"cell": "sink", "port": "D", "event": "posedge clk",
                                                     "delay": [0.1, 0.2]},
                                                    {"cell": "sink", "port": "D", "event": "posedge clk",
                                                     "delay": [0.3, 0.4]}]}]}


class CheckpointTest(unittest.TestCase):
    def setUp(self):
        self.original = fixture()
        self.restored = copy.deepcopy(self.original)
        self.module = self.restored["modules"]["top"]

    def test_signal_numbers_and_route_order_are_not_geometry(self):
        for port in self.module["ports"].values():
            port["bits"] = [bit + 100 for bit in port["bits"]]
        for net in self.module["netnames"].values():
            net["bits"] = [bit + 100 for bit in net["bits"]]
        for cell in self.module["cells"].values():
            for name, bits in cell["connections"].items():
                cell["connections"][name] = [bit + 100 for bit in bits]
        self.module["netnames"]["pad"]["attributes"]["ROUTING"] = "root;;5;sink;pip;5"
        result = assert_preserved(self.original, self.restored)
        self.assertEqual(result["cells"], 1)
        self.assertEqual(result["named_net_bits"], 3)
        self.assertEqual(result["port_bits"], 1)

    def test_analysis_settings_may_change(self):
        self.module["settings"] = {"timing.reanalysis.model": "modern_ff"}
        assert_preserved(self.original, self.restored)

    def test_all_cell_properties_are_preserved(self):
        changes = [("type", "LUT4"), ("parameters", {"INIT": "1"}),
                   ("attributes", {"NEXTPNR_BEL": "X2Y1/DFF0", "BEL_STRENGTH": "101"}),
                   ("attributes", {"NEXTPNR_BEL": "X1Y1/DFF0", "BEL_STRENGTH": "100"}),
                   ("connections", {"D": [11], "Q": [10]}),
                   ("port_directions", {"D": "output", "Q": "input"})]
        for key, value in changes:
            with self.subTest(key=key, value=value):
                changed = copy.deepcopy(self.original)
                changed["modules"]["top"]["cells"]["ff"][key] = value
                with self.assertRaisesRegex(ValueError, "checkpoint changed: cells"):
                    assert_preserved(self.original, changed)

    def test_disconnected_cell_port_descriptor_is_preserved(self):
        self.original["modules"]["top"]["cells"]["ff"]["connections"]["unused"] = []
        self.original["modules"]["top"]["cells"]["ff"]["port_directions"]["unused"] = "output"
        with self.assertRaisesRegex(ValueError, "checkpoint changed: cells"):
            assert_preserved(self.original, self.restored)

    def test_unnamed_dummy_bus_signals_fail_closed(self):
        self.module["cells"]["ff"]["connections"]["D"] = [999]
        with self.assertRaisesRegex(ValueError, "unnamed signal"):
            assert_preserved(self.original, self.restored)

    def test_missing_unused_net_is_not_ignored(self):
        del self.module["netnames"]["unused"]
        with self.assertRaisesRegex(ValueError, "checkpoint changed: nets"):
            assert_preserved(self.original, self.restored)

    def test_missing_port_is_not_ignored(self):
        self.module["ports"] = {}
        with self.assertRaisesRegex(ValueError, "checkpoint changed: ports"):
            assert_preserved(self.original, self.restored)

    def test_route_pip_strength_root_and_wire_are_preserved(self):
        for text in ("sink;other;5;root;;5", "sink;pip;4;root;;5", "sink;pip;5;other;;5"):
            with self.subTest(text=text):
                self.module["netnames"]["pad"]["attributes"]["ROUTING"] = text
                with self.assertRaisesRegex(ValueError, "checkpoint changed: nets"):
                    assert_preserved(self.original, self.restored)

    def test_net_attributes_and_aliases_are_preserved(self):
        self.module["netnames"]["unused"]["attributes"]["src"] = "changed"
        with self.assertRaisesRegex(ValueError, "checkpoint changed: nets"):
            assert_preserved(self.original, self.restored)
        self.module["netnames"]["alias"] = copy.deepcopy(self.module["netnames"]["q"])
        with self.assertRaises(ValueError):
            assert_preserved(self.original, self.restored)

    def test_bus_cell_ports_can_be_serialized_as_scalar_bits(self):
        self.original["modules"]["top"]["cells"]["ff"]["connections"] = {"DO": [10, 11]}
        self.original["modules"]["top"]["cells"]["ff"]["port_directions"] = {"DO": "output"}
        self.module["cells"]["ff"]["connections"] = {"DO[0]": [10], "DO[1]": [11]}
        self.module["cells"]["ff"]["port_directions"] = {"DO[0]": "output", "DO[1]": "output"}
        assert_preserved(self.original, self.restored)

    def test_malformed_routes_fail(self):
        for text in ("wire;pip", "wire;;bad", ";;5", "wire;;5;wire;;5", "wire;;5;tail"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                routes(text)

    def test_missing_route_attribute_fails(self):
        del self.module["netnames"]["unused"]["attributes"]["ROUTING"]
        with self.assertRaisesRegex(ValueError, "missing ROUTING"):
            assert_preserved(self.original, self.restored)

    def test_duplicate_json_keys_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duplicate.json"
            path.write_text('{"modules": {}, "modules": {}}')
            with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
                load(path)


class ReportTest(unittest.TestCase):
    def test_all_fields_reproduce_after_order_normalization(self):
        reference = report_fixture()
        restored = copy.deepcopy(reference)
        restored["detailed_net_timings"][0]["endpoints"].reverse()
        self.assertTrue(assert_report_reproduced(reference, restored)["all_report_fields_reproduced"])

    def test_missing_fmax_fails(self):
        report = report_fixture()
        report["fmax"] = {}
        with self.assertRaisesRegex(ValueError, "missing Fmax"):
            assert_report_reproduced(report_fixture(), report)

    def test_different_model_or_increment_is_not_self_reproduction(self):
        for section in ("fmax", "critical_paths", "detailed_net_timings"):
            with self.subTest(section=section):
                report = report_fixture()
                if section == "fmax":
                    report[section]["clk"]["achieved"] = 99.0
                elif section == "critical_paths":
                    report[section][0]["path"][0]["delay"] = 0.4
                else:
                    report[section][0]["endpoints"][0]["delay"][0] = 0.09
                with self.assertRaisesRegex(ValueError, "report value differs"):
                    assert_report_reproduced(report_fixture(), report)

    def test_nonfinite_results_fail(self):
        report = report_fixture()
        report["fmax"]["clk"]["achieved"] = float("nan")
        with self.assertRaises(ValueError):
            assert_report_reproduced(report_fixture(), report)


if __name__ == "__main__":
    unittest.main()
