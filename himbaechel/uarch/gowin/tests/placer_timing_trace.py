#!/usr/bin/env python3
"""Native HeAP trace transparency/control regression; synthetic timing only.

No vendor tools, bitstreams or hardware. Trace scores are placement estimates,
not routed STA or physical timing acceptance.
"""
import argparse
import json
import math
import os
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace

from cell_timing import PART, characterize
from timing_reload import assert_preserved, assert_report_reproduced, load, require
from timing_reload_flow import synthetic_sidecar


def check_trace(path):
    frames = [json.loads(line) for line in Path(path).read_text().splitlines()]
    require(frames and frames[0]["stage"] == "setup_unplaced" and frames[-1]["stage"] == "after_refine",
            "incomplete placement trace")
    legal = [frame for frame in frames if frame["stage"] == "legal_iteration"]
    retained = [frame for frame in frames if frame["stage"] == "retained_solution"]
    require(legal and len(retained) == 1, "missing legal/retained placement trace")
    require(retained[0]["hpwl"] == min(frame["hpwl"] for frame in legal), "trace changed HPWL retention policy")
    for frame in frames:
        require(frame["verdict"] == "PLACEMENT_HEURISTIC_DIAGNOSTIC_NOT_ROUTED_OR_PHYSICAL_TIMING",
                "trace must retain its heuristic-only qualification")
        require(frame["selection_policy"] == "MIN_HPWL", "unexpected placement selection policy")
        if frame["stage"] in ("legal_iteration", "retained_solution"):
            require(frame["hpwl_basis"] == "HEAP_LEGAL_OBJECTIVE" and math.isfinite(frame["hpwl"]),
                    "missing legal-objective HPWL")
        else:
            require(frame["hpwl_basis"] == "UNAVAILABLE" and frame["hpwl"] is None,
                    "unplaced/post-refinement HPWL must not use stale HeAP coordinates")
        ratio = frame["predicted_worst_target_ratio"]
        require(ratio is None or math.isfinite(ratio), "nonfinite trace score")
        require(frame["sinks"], "missing sink criticality trace")
        seen = set()
        for sink in frame["sinks"]:
            key = (sink["cell"], sink["port"])
            require(key not in seen, "duplicate sink in trace")
            seen.add(key)
            value = sink["criticality"]
            require(value is None or math.isfinite(value) and 0 <= value <= 1, "invalid criticality")
            for key in ("predicted_route_ns", "predicted_setup_slack_ns"):
                value = sink[key]
                require(value is None or math.isfinite(value), "nonfinite predicted timing value")
            # Signed holds are intentional; missing values must remain null,
            # not get silently converted to zero or a timing acceptance claim.
            for clock in sink["queried_clocking"]:
                require(math.isfinite(clock["setup_max_ns"]) and math.isfinite(clock["hold_max_ns"]),
                        "nonfinite queried FF constraint")
    return frames


def run_tests(args, root):
    sidecar = root / "SYNTHETIC_NOT_FOR_PROGRAMMING.json"
    sidecar.write_text(json.dumps(synthetic_sidecar()) + "\n")
    for model in ("legacy", "modern"):
        out = root / model
        out.mkdir()
        trace = out / "heap.jsonl"
        wrapper = out / "trace-backend"
        wrapper.write_text("#!/usr/bin/env python3\nimport subprocess,sys\n"
                           f"raise SystemExit(subprocess.call([{args.nextpnr!r}]+sys.argv[1:]+"
                           f"['--vopt',{('placer_timing_trace=' + str(trace))!r}]))\n")
        wrapper.chmod(0o755)
        common = {"yosys": args.yosys, "env": {}, "chipdb": args.chipdb,
                  "modern_ff_timing": sidecar if model == "modern" else None, "gowin": None}
        characterize(SimpleNamespace(out=out / "default", nextpnr=args.nextpnr, **common), "ff_ce")
        characterize(SimpleNamespace(out=out / "traced", nextpnr=str(wrapper), **common), "ff_ce")
        original, traced = out / "default/ff_ce", out / "traced/ff_ce"
        assert_preserved(load(original / "pnr.json"), load(traced / "pnr.json"))
        assert_report_reproduced(load(original / "native-report.json"), load(traced / "native-report.json"))
        frames = check_trace(trace)
        before = trace.read_bytes()
        rejected = out / "rejected"
        rejected.mkdir()
        command = [args.nextpnr, "--device", PART, "--json", str(original / "top.json"),
                   "--vopt", "family=GW5A-25A", "--vopt", "cst=" + str(original / "fixture.cst"),
                   "--vopt", "independent_ff", "--vopt", "placer_timing_trace=" + str(trace),
                   "--sdc", str(original / "native.sdc"), "--timing-allow-fail",
                   "--write", str(rejected / "pnr.json"), "--report", str(rejected / "report.json")]
        if args.chipdb:
            command += ["--chipdb", str(args.chipdb)]
        if model == "modern":
            command += ["--vopt", "modern_ff_timing=" + str(sidecar)]
        with (rejected / "driver.log").open("w") as stream:
            result = subprocess.run(command, cwd=rejected, stdout=stream, stderr=subprocess.STDOUT, check=False)
        require(result.returncode != 0 and not (rejected / "pnr.json").exists() and
                not (rejected / "report.json").exists(), "existing trace was not rejected before implementation output")
        require(trace.read_bytes() == before, "existing trace was overwritten")
        print(f"{model}: trace transparency, {len(frames)} trace frames and clobber rejection passed")
    print("PLACER_TIMING_TRACE_CONTROLS_PASSED_NOT_PHYSICAL_TIMING_ACCEPTANCE")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nextpnr", default=os.environ.get("NEXTPNR_HIMBAECHEL", "nextpnr-himbaechel"))
    parser.add_argument("--yosys", default=os.environ.get("YOSYS", "yosys"))
    parser.add_argument("--chipdb", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--check", type=Path)
    args = parser.parse_args()
    if args.check:
        frames = check_trace(args.check)
        print(f"{len(frames)} heuristic-only trace frames checked")
        return
    if args.chipdb:
        args.chipdb = args.chipdb.resolve()
    if args.out:
        args.out = args.out.resolve()
        args.out.mkdir(parents=True, exist_ok=False)
        run_tests(args, args.out)
    else:
        with tempfile.TemporaryDirectory(prefix="gowin-placer-trace-") as directory:
            run_tests(args, Path(directory))


if __name__ == "__main__":
    main()
