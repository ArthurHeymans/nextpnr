#!/usr/bin/env python3
"""Vendor-independent tests for fixed-placement characterization/report parsing."""
import io
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest.mock import patch

from cell_timing import (
    CASES,
    PART,
    Tables,
    fixture,
    io_cell_arcs,
    main,
    parse_pins,
    parse_vendor,
    vendor_location,
)


def table(rows):
    return "<table>" + "".join("<tr>" + "".join(f"<td>{cell}</td>" for cell in row) + "</tr>" for row in rows) + "</table>"


def report():
    metadata = table([["Part Number", PART], ["Device Version", "B"],
                      ["Setup Delay Model", "Slow 0.855V 0C C1/I0"],
                      ["Hold Delay Model", "Fast 0.945V 85C C1/I0"]])
    header = ["AT", "DELAY", "TYPE", "RF", "FANOUT", "LOC", "NODE"]
    text = metadata
    for mode, signed, kind in (("setup", "-0.291", "tSu"), ("hold", "-0.053", "tHld")):
        text += f"<h4>Report Command:report_timing -{mode} -max_paths 25 -max_common_paths 1</h4>"
        text += table([["Path Number", "From Node", "To Node"], ["1", "source/Q", "sink/CE"]])
        text += "<h3>Path1</h3>"
        text += table([["Slack", "8.998"], ["Data Arrival Time", "3.349"],
                       ["Data Required Time", "12.347"], ["From", "source"], ["To", "sink"]])
        text += table([header, ["0.683", "0.683", "tINS", "RR", "2", "IOB12[A]", "clk_IBUF_I/O"],
                       ["3.016", "0.368", "tC2Q", "RF", "1", "R2C12[0][A]", "source/Q"],
                       ["3.349", "0.334", "tNET", "FF", "1", "R2C11[0][A]", "sink/CE"]])
        text += table([header, ["12.347", signed, kind, "", "1", "R2C11[0][A]", "sink"]])
    return text


def scoped_report(missing=False):
    text = report().split('<h4>Report Command:', 1)[0]
    scopes = [('report_timing -setup -from [get_ports {data}]', 'input', 'capture'),
              ('report_timing -setup -from [get_regs {source}]', 'source', 'sink')]
    # Vendor emits every summary first, then details with repeated command
    # headings. Path numbering restarts within each scope, even in one mode.
    for index, (command, source, sink) in enumerate(scopes):
        text += '<h4>Report Command:' + command + '</h4>'
        if not (missing and index == 0):
            text += table([['Path Number', 'From Node', 'To Node'],
                           ['1', source + '/O', sink + '/D']])
    for command, source, sink in scopes:
        text += '<h4>Report Command:' + command + '</h4><h3>Path1</h3>'
        text += table([['Slack', '1.0'], ['From', source], ['To', sink]])
        text += table([['AT', 'DELAY', 'TYPE', 'RF', 'FANOUT', 'LOC', 'NODE'],
                       ['0.7', '0.7', 'tINS', 'RR', '1', 'IOB1[A]', source + '/O']])
    return text


class CellTimingTest(unittest.TestCase):
    def test_repeated_path_numbers_are_scoped_by_report_command(self):
        parsed = parse_vendor(scoped_report())
        self.assertEqual([p['number'] for p in parsed['paths']], [1, 1])
        self.assertEqual([p['endpoints']['From Node'] for p in parsed['paths']], ['input/O', 'source/O'])
        self.assertEqual([a['from_node'] for a in parsed['arcs']], ['input/O', 'source/O'])

    def test_unqualified_port_endpoint_identity(self):
        parsed = parse_vendor(scoped_report().replace('input/O', 'input'))
        self.assertEqual(parsed['paths'][0]['endpoints']['From Node'], 'input')

    def test_missing_scoped_endpoints_are_not_borrowed_from_another_scope(self):
        with self.assertRaisesRegex(ValueError, 'no matching command-scoped'):
            parse_vendor(scoped_report(missing=True))

    def test_disagreeing_summary_instance_fails_closed(self):
        with self.assertRaisesRegex(ValueError, 'endpoints disagree'):
            parse_vendor(scoped_report().replace('<td>input</td>', '<td>wrong_instance</td>', 1))

    def test_conflicting_endpoints_within_same_command_fail_closed(self):
        text = scoped_report().replace('report_timing -setup -from [get_regs {source}]',
                                       'report_timing -setup -from [get_ports {data}]')
        with self.assertRaisesRegex(ValueError, 'Conflicting path endpoints'):
            parse_vendor(text)

    def test_exception_report_does_not_inherit_last_timing_mode(self):
        text = report() + '<h4>Report Command:report_exceptions -setup</h4><h3>Path1</h3>'
        text += table([['Slack', '-99'], ['From', 'unrelated'], ['To', 'other']])
        parsed = parse_vendor(text)
        self.assertEqual(len(parsed['paths']), 2)
        self.assertEqual([p['analysis'] for p in parsed['paths']], ['setup', 'hold'])

    def test_delays_are_not_timestamps(self):
        parsed = parse_vendor(report())
        cq = [arc for arc in parsed["arcs"] if arc["type"] == "tC2Q"]
        self.assertEqual([arc["delay_ns"] for arc in cq], [0.368, 0.368])
        self.assertEqual(cq[0]["at_ns"], "3.016")
        self.assertEqual(cq[0]["rf"], "RF")

    def test_modes_survive_path_headings(self):
        parsed = parse_vendor(report())
        self.assertEqual([path["analysis"] for path in parsed["paths"]], ["setup", "hold"])
        self.assertEqual(parsed["paths"][0]["number"], 1)
        self.assertEqual(len(parsed["report_commands"]), 2)

    def test_signed_required_increments_and_pin_identity(self):
        parsed = parse_vendor(report())
        setup = next(arc for arc in parsed["arcs"] if arc["type"] == "tSu")
        hold = next(arc for arc in parsed["arcs"] if arc["type"] == "tHld")
        self.assertEqual(setup["delay_ns"], -0.291)
        self.assertEqual(hold["delay_ns"], -0.053)
        self.assertEqual(setup["from_node"], "source/Q")
        self.assertEqual(setup["to_node"], "sink/CE")
        self.assertEqual(setup["from"], "source")

    def test_missing_corner_is_not_a_default(self):
        with self.assertRaisesRegex(ValueError, "Missing vendor metadata"):
            parse_vendor(report().replace("Hold Delay Model", "Missing Model"))

    def test_empty_report_is_not_a_pass(self):
        with self.assertRaises(ValueError):
            parse_vendor("<table></table>")

    def test_nonfinite_arc_rejected(self):
        with self.assertRaisesRegex(ValueError, "Non-finite"):
            parse_vendor(report().replace("0.368", "NaN"))

    def test_unclosed_vendor_frequency_cell(self):
        parser = Tables()
        parser.feed("<table><tr><td>clk</td><td>100.000<td>0.000</td></tr></table>")
        self.assertEqual(parser.tables[0]["rows"], [["clk", "100.000", "0.000"]])

    def test_io_selection_uses_instance_type_not_location_prefix(self):
        rows = [{"type": "tINS", "node": name + "/O", "location": location}
                for name, location in (("top/a", "IOT7[A]"), ("b", "IOL9[A]"),
                                       ("clk", "IOB12[A]"), ("d", "IOR3[A]"))]
        types = dict.fromkeys(("top/a", "b", "clk", "d"), "IBUF")
        self.assertEqual(io_cell_arcs({"arcs": rows}, types), rows)

    def test_io_selection_excludes_synthetic_constraints_and_lut_arcs(self):
        rows = [{"type": "tC2Q", "node": "bus/IO", "delay_ns": 2.0},
                {"type": "tOut", "node": "bus", "delay_ns": -1.0},
                {"type": "tINS", "node": "lut/F", "delay_ns": 0.4},
                {"type": "tINS", "node": "bus/O", "delay_ns": 0.68},
                {"type": "tINS", "node": "bus/IO", "delay_ns": 0.0}]
        self.assertEqual(io_cell_arcs({"arcs": rows}, {"bus": "IOBUF", "lut": "LUT4"}), rows[3:])

    def test_io_selection_does_not_guess_unknown_or_missing_types(self):
        rows = [{"type": "tINS", "node": "q_OBUF_O/O"}]
        self.assertEqual(io_cell_arcs({"arcs": rows}, {}), [])
        self.assertEqual(io_cell_arcs({"arcs": rows}, {"q_OBUF_O": "UNKNOWN"}), [])

    def test_actual_pin_attributes_not_just_cst(self):
        text = table([["Port Name", "Loc./Bank", "IO Type", "Drive"],
                      ["q", "C3/4", "LVCMOS33", "8"]])
        self.assertEqual(parse_pins(text)["q"]["Drive"], "8")
        with self.assertRaises(ValueError):
            parse_pins("<table></table>")

    def test_fixture_location_maps_and_explicit_constraints(self):
        self.assertEqual(len(CASES), 23)
        for name in CASES:
            with self.subTest(name=name):
                verilog, cells = fixture(name)
                self.assertEqual(cells["source"], "X11Y1/DFF0")
                self.assertEqual(cells["sink"], "X10Y1/DFF0")
                self.assertIn(".Q(q)", verilog)
                for cell, bel in cells.items():
                    if not ((name.startswith("ff_alu") and cell in ("alu", "sink")) or cell == "ram"):
                        self.assertIn(f'BEL="{bel}"', verilog, cell)
                    pattern = r"^R\d+C\d+$" if cell == "ram" else r"^R\d+C\d+\[\d+\]\[[AB]\]$"
                    self.assertRegex(vendor_location(bel), pattern)

    def test_distinct_control_sources_and_lut_data_path(self):
        verilog, cells = fixture("ff_reset_ce")
        self.assertIn(".CE(ce_q)", verilog)
        self.assertIn(".RESET(reset_q)", verilog)
        self.assertIn("enable_source", cells)
        self.assertIn("reset_source", cells)
        self.assertNotEqual(cells["enable_source"], cells["reset_source"])
        self.assertIn(".D(lut_d)", fixture("ff_lut")[0])
        self.assertEqual(len(fixture("ff_fanout")[1]), 7)

    def test_recovery_and_removal_are_not_setup_and_hold(self):
        text = report().replace("-setup -max_paths", "-recovery -max_paths")
        text = text.replace("-hold -max_paths", "-removal -max_paths")
        parsed = parse_vendor(text)
        self.assertEqual([p["analysis"] for p in parsed["paths"]], ["recovery", "removal"])
        self.assertTrue(any(a["analysis"] == "recovery" and a["type"] == "tSu" for a in parsed["arcs"]))

    def test_existing_evidence_cannot_be_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "result.json"
            marker.write_text("preserve me")
            with (
                patch("sys.argv", ["cell_timing", "--out", directory]),
                redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit) as error,
            ):
                main()
            self.assertEqual(error.exception.code, 2)
            self.assertEqual(marker.read_text(), "preserve me")

    def test_set_and_preset_have_distinct_pins_and_data_modes(self):
        for name, pin, kind in (("ff_set_ce", "SET", "DFFSE"), ("ff_preset_ce", "PRESET", "DFFPE")):
            for suffix in ("", "_lut"):
                verilog, cells = fixture(name + suffix)
                self.assertIn(f"{kind} sink", verilog)
                self.assertIn(f".{pin}(reset_q)", verilog)
                self.assertIn(".CE(ce_q)", verilog)
                self.assertIn("reset_source", cells)
                self.assertIn(".D(lut_d)" if suffix else ".D(source_q)", verilog)

    def test_alu_fixture_reserves_local_sum_and_both_clock_edges(self):
        for name in ("ff_alu", "ff_alu_neg", "ff_alu_reset_ce"):
            verilog, cells = fixture(name)
            self.assertEqual(cells["alu"], "X10Y1/ALU0")
            self.assertNotEqual(cells["unrelated"], "X10Y1/LUT0")
            self.assertIn(".SUM(alu_d)", verilog)
            self.assertIn(".D(alu_d)", verilog)
            self.assertEqual(vendor_location(cells["alu"]), vendor_location(cells["sink"]))
        self.assertIn("DFFN sink", fixture("ff_alu_neg")[0])
        self.assertIn(".RESET(reset_q)", fixture("ff_alu_reset_ce")[0])

    def test_remote_alu_fixture_reserves_unrelated_lut_at_sink(self):
        for name in ("ff_alu_remote", "ff_alu_remote_reset_ce"):
            verilog, cells = fixture(name)
            self.assertEqual(cells["unrelated"], "X10Y1/LUT0")
            self.assertIn('BEL="X10Y1/DFF0"', verilog)
            self.assertIn(".D(alu_d)", verilog)

    def test_ram_fixture_observes_all_lanes_and_uses_separate_select(self):
        for name in ("ff_ram", "ff_ram_reset_ce"):
            verilog, cells = fixture(name)
            self.assertIn("RAM16SDP4", verilog)
            self.assertIn(".D(ram_do[0])", verilog)
            for i in range(1, 4):
                self.assertIn(f"ram_do[{i}]", verilog)
            self.assertEqual(vendor_location(cells["ram"]), "R2C9")

    def test_invalid_bel_rejected(self):
        with self.assertRaises(ValueError):
            vendor_location("X10Y1/DSP0")


if __name__ == "__main__":
    unittest.main()
