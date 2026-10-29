#!/usr/bin/env python3
"""Pinned LUT/register and shared-SEL legality regressions (GW5A-25A)."""
import json
import os
import subprocess
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
yosys = os.environ.get("YOSYS", "yosys")
nextpnr = os.environ.get("NEXTPNR_HIMBAECHEL", "nextpnr-himbaechel")

cases = [
    ([], [], False, None),
    ([], ["ff_d_from_sel"], False, None),
    ([], ["independent_ff"], True, "SEL0"),
    (["PAIRED"], [], True, "F0"),
    (["CONSTANT"], ["independent_ff"], False, None),
    (["WIDE"], ["independent_ff"], False, None),
    (["WIDE", "SHARED_SELECT"], ["independent_ff"], True, "SEL0"),
]

for defines, options, succeeds, source in cases:
    with tempfile.TemporaryDirectory() as directory:
        inp = Path(directory) / "input.json"
        out = Path(directory) / "routed.json"
        subprocess.run([
            yosys, "-Q", "-T", "-q", "-p",
            'read_verilog -lib +/gowin/cells_sim.v; '
            f'read_verilog {" ".join("-D" + d for d in defines)} "{here / "independent_ff.v"}"; '
            f'hierarchy -top top; synth_gowin -family gw5a -nolutram -nowidelut -top top -json "{inp}"',
        ], check=True)
        args = [nextpnr, "--json", str(inp), "--write", str(out),
                "--device", "GW5A-LV25MG121NC1/I0", "--vopt", "family=GW5A-25A",
                "--vopt", f"cst={here / 'independent_ff.cst'}", "--timing-allow-fail"]
        for option in options:
            args += ["--vopt", option]
        result = subprocess.run(args, capture_output=True, text=True)
        log = result.stdout + result.stderr
        assert (result.returncode == 0) == succeeds, (defines, options, log)
        if not succeeds:
            assert "ERROR:" in log and "Unconstrained IO" not in log, log
            continue
        module = json.loads(out.read_text())["modules"]["top"]
        ff = module["cells"]["reg_d"]
        assert ff["attributes"]["NEXTPNR_BEL"] == "X10Y1/DFF0"
        d_bit = ff["connections"]["D"][0]
        # Check the route of this register's D, not an unrelated alias/net.
        pips = {pip for net in module["netnames"].values() if d_bit in net["bits"]
                for pip in net.get("attributes", {}).get("ROUTING", "").split(";")}
        assert f"X10Y1/XD0/{source}" in pips, (defines, options, pips)
        assert not any("_passthrough_lut$" in name for name in module["cells"])
        print(f"PASS independent FF: defines={defines}, options={options}")
