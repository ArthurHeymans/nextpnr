#!/usr/bin/env python3
"""Native-only timing reload regression; synthetic timing, never programming.

Requires Yosys and a GW5A-25A nextpnr build. Runs two tiny fresh routes, then
checks exact checkpoint/report reproduction and fail-closed controls. No vendor
PnR, bitstream generation, hardware access or physical timing assertion.
"""
import argparse
import copy
import json
import os
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace

from cell_timing import PART, characterize
from timing_reload import assert_preserved, assert_report_reproduced, load, require


def synthetic_sidecar():
    records = {}
    for edge in ("Pos", "Neg"):
        for mode in ("DI_M", "DI_FS", "CE", "LSR_SYN", "LSR_ASYN"):
            records[mode + "_" + edge] = {"setup_ps": [10, 20] if mode == "DI_FS" else [200, 300],
                                          "hold_ps": [10, 25] if mode == "DI_FS" else [-70, -50]}
        records["CLK_Q_" + edge] = {"clock_q_ps": [100, 400] if edge == "Pos" else [150, 450]}
    return {"schema": "gowin-modern-ff-v1", "profile": "GW5A25-V1.9.11.03-default-C1/I0",
            "target_arch": "GW5A-25A", "target_part": PART, "source_device": "GW5A-25A",
            "source_sha256": "5e723783372c005a584e7e502646675d68a83256d28018ab82b4a88bad8f235a",
            "normalization": "slow_group_1_x1.25_fast_group_2_x1_outward_ps",
            "aggregation": "all_four_source_slots_not_RF_resolved", "records": records}


def run_tests(args, root):
    sidecar = root / "SYNTHETIC_NOT_FOR_PROGRAMMING.json"
    sidecar.write_text(json.dumps(synthetic_sidecar()) + "\n")
    for model in ("legacy", "modern"):
        fresh_root = root / model
        fixture_args = SimpleNamespace(out=fresh_root, yosys=args.yosys, nextpnr=args.nextpnr, env={},
                                       chipdb=args.chipdb, modern_ff_timing=sidecar if model == "modern" else None,
                                       gowin=None)
        characterize(fixture_args, "ff_ce")
        fresh = fresh_root / "ff_ce"
        original = load(fresh / "pnr.json")
        reference = load(fresh / "native-report.json")
        require(len(reference["fmax"]) == 1, "unexpected fixture clock domains")
        clock, frequency = next(iter(reference["fmax"].items()))
        require(frequency["constraint"] == 100, "fixture clock is not 100 MHz")
        sdc = fresh_root / "reload.sdc"
        sdc.write_text(f"create_clock -name clk -period 10.000 [get_nets {{{clock}}}]\n")
        base = [args.nextpnr, "--timing-only", "--no-pack", "--no-place", "--no-route", "--device", PART,
                "--vopt", "family=GW5A-25A", "--vopt", "independent_ff", "--sdc", str(sdc),
                "--timing-allow-fail", "--detailed-timing-report"]
        if args.chipdb:
            base += ["--chipdb", str(args.chipdb)]
        if model == "modern":
            base += ["--vopt", "modern_ff_timing=" + str(sidecar)]

        def invoke(name, design=None, remove=None, extra=(), clobber=False,
                   fresh_root=fresh_root, original=original, base=base):
            out = fresh_root / name
            out.mkdir()
            source = out / "input.json"
            source.write_text(json.dumps(original if design is None else design) + "\n")
            before = source.read_bytes()
            command = base + ["--json", str(source), "--write", str(out / "roundtrip.json"),
                              "--report", str(out / "report.json")]
            if remove:
                command.remove(remove)
            command += list(extra)
            if clobber:
                command += ["--log", str(source)]
            (out / "command.json").write_text(json.dumps(command, indent=2) + "\n")
            with (out / "driver.log").open("w") as stream:
                result = subprocess.run(command, cwd=out, stdout=stream, stderr=subprocess.STDOUT, check=False)
            require(source.read_bytes() == before, "input checkpoint was overwritten")
            return out, result.returncode

        out, rc = invoke("reload")
        require(rc == 0, (out / "driver.log").read_text())
        assert_preserved(original, load(out / "roundtrip.json"))
        assert_report_reproduced(reference, load(out / "report.json"))
        settings = load(out / "roundtrip.json")["modules"]["top"]["settings"]
        require(settings["timing.reanalysis.pip_model"] == "HIMBAECHEL_ROUTER_FAST_PIP_DELAYS", "wrong pip model")
        require(settings["timing.reanalysis.model"] == ("modern_ff" if model == "modern" else "legacy_chipdb"),
                "wrong active FF model")

        def rejected(name, design=None, **options):
            out, rc = invoke(name, design, **options)
            require(rc != 0 and not (out / "report.json").exists() and not (out / "roundtrip.json").exists(),
                    "invalid reload accepted: " + name)

        rejected("missing-no-route", remove="--no-route")
        rejected("force-forbidden", extra=("--force",))
        rejected("log-must-not-clobber-input", clobber=True)
        for setting, value in (("route", "0"), ("target_freq", "nan")):
            changed = copy.deepcopy(original)
            changed["modules"]["top"]["settings"][setting] = value
            rejected("invalid-" + setting, changed)
        changed = copy.deepcopy(original)
        changed["modules"]["top"]["cells"]["sink"]["attributes"].pop("NEXTPNR_BEL")
        rejected("missing-bel", changed)
        changed = copy.deepcopy(original)
        module = changed["modules"]["top"]
        bit = module["cells"]["sink"]["connections"]["D"][0]
        for net in module["netnames"].values():
            if net["bits"] == [bit]:
                fields = net["attributes"]["ROUTING"].split(";")
                triples = [fields[i:i + 3] for i in range(0, len(fields), 3)]
                require(any(not triple[1] for triple in triples), "fixture data net has no saved root")
                net["attributes"]["ROUTING"] = ";".join(field for triple in triples if triple[1] for field in triple)
        rejected("missing-data-route-root", changed)
        print(model + ": original report/geometry reproduced; seven invalid reloads rejected")
    print("TIMING_RELOAD_FLOW_PASSED_SYNTHETIC_ONLY_NOT_PHYSICAL_ACCEPTANCE")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nextpnr", default=os.environ.get("NEXTPNR_HIMBAECHEL", "nextpnr-himbaechel"))
    parser.add_argument("--yosys", default=os.environ.get("YOSYS", "yosys"))
    parser.add_argument("--chipdb", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.chipdb:
        args.chipdb = args.chipdb.resolve()
    if args.out:
        args.out = args.out.resolve()
        args.out.mkdir(parents=True, exist_ok=False)
        run_tests(args, args.out)
    else:
        with tempfile.TemporaryDirectory(prefix="gowin-timing-reload-") as directory:
            run_tests(args, Path(directory))


if __name__ == "__main__":
    main()
