#!/usr/bin/env python3
"""Check generated BRAM timing coverage without depending on vendor delays."""
import importlib.util
import sys
import unittest
from collections import defaultdict
from pathlib import Path
from types import SimpleNamespace

path = Path(__file__).resolve().parents[1] / "gowin_arch_gen.py"
spec = importlib.util.spec_from_file_location("gowin_arch_gen_test", path)
assert spec is not None and spec.loader is not None
generator = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = generator
spec.loader.exec_module(generator)


class TimingCell:
    def __init__(self):
        self.inputs = set()

    def add_setup_hold(self, clock, pin, *args):
        self.inputs.add((clock, pin))

    def add_clock_out(self, *args):
        pass


class TimingPool:
    def __init__(self):
        self.cells = {}

    def add_cell_variant(self, speed, name):
        cell = TimingCell()
        self.cells[name] = cell
        return cell


class BramTimingTest(unittest.TestCase):
    def test_dual_port_b_address_is_timed_on_dual_port_cells(self):
        timing = TimingPool()
        chip = SimpleNamespace(set_speed_grades=lambda speeds: timing)
        arcs = defaultdict(lambda: [0.1] * 4)
        db = SimpleNamespace(timing={"test": {"bram": arcs}})
        generator.create_timing_info(chip, db)
        for name in ("DP", "DPX9", "DPB", "DPX9B"):
            for i in range(14):
                self.assertIn(("CLKB", f"ADB{i}"), timing.cells[name].inputs, name)
        for name in ("SP", "SPX9"):
            self.assertFalse(any(clock == "CLKB" for clock, _ in timing.cells[name].inputs), name)


if __name__ == "__main__":
    unittest.main()
