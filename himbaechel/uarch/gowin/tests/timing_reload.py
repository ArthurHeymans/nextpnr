#!/usr/bin/env python3
"""Check exact logical/physical checkpoint preservation and timing self-controls.

Signal numbers and ROUTING iteration order are serialization details. All named
net aliases, connections, cell types/parameters/attributes, ports, BELs, route
wires/pips/strengths and module attributes must otherwise agree. Settings are
excluded: they describe the newly selected analysis, not physical geometry.
This checker does not establish physical timing acceptance.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key: " + key)
            result[key] = value
        return result
    return json.loads(Path(path).read_text(), object_pairs_hook=unique)


def bit_names(name, width, offset=0, upto=False):
    return [name if width == 1 and offset == 0 else
            f"{name}[{offset + (width - i - 1 if upto else i)}]"
            for i in range(width)]


def routes(text):
    require(isinstance(text, str), "ROUTING must be a string")
    if not text.strip():
        return []
    fields = text.split(";")
    require(len(fields) % 3 == 0, "incomplete ROUTING triple")
    triples = [fields[i:i + 3] for i in range(0, len(fields), 3)]
    require(all(w and s.isdecimal() for w, _p, s in triples), "invalid ROUTING fields")
    require(len({t[0] for t in triples}) == len(triples), "duplicate route wire")
    return sorted(triples)


def checkpoint(design):
    modules = design.get("modules", {})
    require(len(modules) == 1, "expected one flat checkpoint module")
    name, module = next(iter(modules.items()))
    aliases = {}
    for label, net in module["netnames"].items():
        names = bit_names(label, len(net["bits"]), net.get("offset", 0), net.get("upto", False))
        for i, bit in enumerate(net["bits"]):
            bit_name = names[i]
            if isinstance(bit, int):
                aliases.setdefault(bit, []).append(bit_name)

    def signal(bit):
        if isinstance(bit, int):
            require(bit in aliases, "unnamed signal " + str(bit))
            return ["net", sorted(aliases[bit])]
        require(bit in ("0", "1", "x", "z"), "invalid signal value")
        return ["constant", bit]

    def connections(ports, directions=None):
        result = {}
        for port, value in ports.items():
            bits = value["bits"] if directions is None else value
            if not bits:
                if directions is None:
                    raise ValueError("empty top-level port")
                require(port not in result, "duplicate empty cell port")
                result[port] = [directions[port], ["disconnected"]]
            names = bit_names(port, len(bits), value.get("offset", 0) if directions is None else 0,
                              value.get("upto", False) if directions is None else False)
            for i, bit in enumerate(bits):
                bit_name = names[i]
                require(bit_name not in result, "duplicate flattened port " + bit_name)
                direction = value["direction"] if directions is None else directions[port]
                result[bit_name] = [direction, signal(bit)]
        return result

    cells = {}
    for cell_name, cell in module["cells"].items():
        cells[cell_name] = {"type": cell["type"], "parameters": cell.get("parameters", {}),
                            "attributes": cell.get("attributes", {}),
                            "ports": connections(cell["connections"], cell["port_directions"])}
    nets = {}
    for label, net in module["netnames"].items():
        attributes = dict(net.get("attributes", {}))
        require("ROUTING" in attributes, "missing ROUTING on " + label)
        attributes["ROUTING"] = routes(attributes["ROUTING"])
        names = bit_names(label, len(net["bits"]), net.get("offset", 0), net.get("upto", False))
        for i, bit in enumerate(net["bits"]):
            bit_name = names[i]
            require(bit_name not in nets, "duplicate flattened net " + bit_name)
            nets[bit_name] = {"signal": signal(bit), "attributes": attributes}
    return {"module": name, "attributes": module.get("attributes", {}), "cells": cells,
            "ports": connections(module["ports"]), "nets": nets}


def assert_preserved(original, restored):
    before, after = checkpoint(original), checkpoint(restored)
    for section in before:
        require(before[section] == after[section], "checkpoint changed: " + section)
    canonical = json.dumps(before, sort_keys=True, separators=(",", ":")).encode()
    return {"canonical_sha256": hashlib.sha256(canonical).hexdigest(), "cells": len(before["cells"]),
            "named_net_bits": len(before["nets"]), "port_bits": len(before["ports"])}


def canonical_report(report):
    require(report.get("fmax"), "missing Fmax results: writing a report alone is not analysis")
    require(report.get("critical_paths"), "missing critical paths")
    for clock, values in report["fmax"].items():
        for key in ("achieved", "constraint"):
            value = values.get(key)
            require(isinstance(value, (int, float)) and not isinstance(value, bool) and
                    math.isfinite(value) and value > 0, "invalid Fmax value on " + clock)
    for path in report["critical_paths"]:
        require(path.get("path"), "empty critical path")
        require(all(isinstance(segment.get("delay"), (int, float)) and math.isfinite(segment["delay"])
                    for segment in path["path"]), "invalid critical-path increment")
    result = dict(report)
    result["critical_paths"] = sorted(report["critical_paths"], key=lambda p: (p["from"], p["to"]))
    if "detailed_net_timings" in result:
        details = []
        for row in result["detailed_net_timings"]:
            row = dict(row)
            row["endpoints"] = sorted(row["endpoints"], key=lambda e: (e["cell"], e["port"], e["event"], tuple(e["delay"])))
            details.append(row)
        result["detailed_net_timings"] = sorted(details, key=lambda r: (r["driver"], r["port"], r["event"], r["net"]))
    return result


def assert_report_reproduced(reference, restored, tolerance=1e-6):
    def compare(a, b, location):
        if isinstance(a, dict):
            require(isinstance(b, dict) and a.keys() == b.keys(), "report keys differ: " + location)
            for key in a:
                compare(a[key], b[key], location + "/" + key)
        elif isinstance(a, list):
            require(isinstance(b, list) and len(a) == len(b), "report list differs: " + location)
            for i, x in enumerate(a):
                compare(x, b[i], location + "/" + str(i))
        elif isinstance(a, (int, float)) and not isinstance(a, bool):
            require(isinstance(b, (int, float)) and math.isfinite(a) and math.isfinite(b) and
                    abs(a - b) <= tolerance, "report value differs: " + location + ": " + str((a, b)))
        else:
            require(a == b, "report value differs: " + location + ": " + str((a, b)))
    compare(canonical_report(reference), canonical_report(restored), "report")
    return {"all_report_fields_reproduced": True, "absolute_tolerance": tolerance}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("restored", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--reference-report", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = {"preservation": assert_preserved(load(args.original), load(args.restored)),
              "timing_acceptance": None, "physical_bounds": None}
    report = load(args.report)
    canonical_report(report)
    result["fmax_source_model_estimates"] = report["fmax"]
    if args.reference_report:
        result["self_control"] = assert_report_reproduced(load(args.reference_report), report)
        result["verdict"] = "GEOMETRY_AND_ORIGINAL_MODEL_REPORT_REPRODUCED_NOT_PHYSICAL_ACCEPTANCE"
    else:
        result["verdict"] = "GEOMETRY_PRESERVED_REANALYSIS_REQUIRES_SEPARATE_ORIGINAL_MODEL_SELF_CONTROL"
    with args.output.open("x") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(result["verdict"])


if __name__ == "__main__":
    main()
