#!/usr/bin/env python3
"""Integration test; requires Yosys and a nextpnr build with the GW5A-25A database."""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
yosys = os.environ.get("YOSYS", "yosys")
nextpnr = os.environ.get("NEXTPNR_HIMBAECHEL", "nextpnr-himbaechel")

# Pairs packed into the carry chain, and ALU output FFs left with a passthrough
# LUT because their control signals do not match the rest of their column pair.
cases = [
    ("alu_ff.v", 5, 1),
    ("alu_ff_groups.v", 6, 0),
]

for source_name, expected_pairs, expected_skipped in cases:
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "input.json"
        placed = Path(directory) / "placed.json"
        subprocess.run([
            yosys, "-Q", "-T", "-q", "-p",
            f'read_verilog -lib +/gowin/cells_sim.v; read_verilog -sv "{here / source_name}"; '
            f'hierarchy -top top; synth_gowin -family gw5a -top top -json "{source}"',
        ], check=True)
        result = subprocess.run([
            nextpnr, "--json", str(source), "--write", str(placed),
            "--device", "GW5A-LV25MG121NC1/I0", "--vopt", "family=GW5A-25A",
            "--vopt", "pack_alu_ff", "--vopt", f"cst={here / 'alu_ff.cst'}",
            "--timing-allow-fail",
        ], check=True, capture_output=True, text=True)
        output = result.stdout + result.stderr
        match = re.search(r"Constrained (\d+) ALUFF pairs", output)
        assert match and int(match[1]) == expected_pairs, output
        if expected_skipped:
            assert re.search(rf"Left {expected_skipped} ALU output FFs unpacked", output), output
        else:
            assert "Left" not in output, output

        cells = json.loads(placed.read_text())["modules"]["top"]["cells"]
        lut_count = re.search(r"Post-placement LUT cells: (\d+)", output)
        assert lut_count and int(lut_count[1]) == sum(
            cell["type"] in {"LUT1", "LUT2", "LUT3", "LUT4"} for cell in cells.values()
        )
        alu_outputs = {
            cell["connections"]["SUM"][0]: cell["attributes"]["NEXTPNR_BEL"]
            for cell in cells.values()
            if cell["type"] == "ALU" and cell["connections"].get("SUM")
        }
        direct = 0
        for cell in cells.values():
            if cell["type"] != "DFFRE":
                continue
            alu_bel = alu_outputs.get(cell["connections"]["D"][0])
            if alu_bel is not None:
                assert alu_bel.replace("/ALU", "/DFF") == cell["attributes"]["NEXTPNR_BEL"]
                direct += 1
        assert int(match[1]) == direct, output
        assert any("_passthrough_lut$" in name for name in cells) == bool(expected_skipped), output
