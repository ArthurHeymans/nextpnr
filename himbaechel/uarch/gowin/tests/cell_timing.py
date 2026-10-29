#!/usr/bin/env python3
"""Bounded GW5A cell timing characterization, NOT a timing sign-off test.

Uses matching primitive netlists and fixed logic/pad locations in both tools.
Routes differ. Native constraints intentionally omit unsupported external I/O
constraints. Synthetic vendor I/O delays are application probes, not bounds.
Vendor delay-model labels and RF branches are retained without fitting delays.
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
from html.parser import HTMLParser
from pathlib import Path

PART = "GW5A-LV25MG121NC1/I0"
CASES = {
    "ff": ("DFF", "DFF", False, False),
    "ff_neg": ("DFFN", "DFFN", False, False),
    "ff_ce": ("DFF", "DFFE", False, False),
    "ff_reset_ce": ("DFF", "DFFRE", False, False),
    "ff_clear_ce": ("DFF", "DFFCE", False, False),
    "ff_neg_ce": ("DFFN", "DFFNE", False, False),
    "ff_lut": ("DFF", "DFF", True, False),
    "ff_fanout": ("DFF", "DFF", False, True),
    "io_slow4": ("DFF", "DFF", False, False),
    "io_slow8": ("DFF", "DFF", False, False),
    "io_fast4": ("DFF", "DFF", False, False),
    "io_fast8": ("DFF", "DFF", False, False),
    "ff_set_ce": ("DFF", "DFFSE", False, False),
    "ff_preset_ce": ("DFF", "DFFPE", False, False),
    "ff_set_ce_lut": ("DFF", "DFFSE", True, False),
    "ff_preset_ce_lut": ("DFF", "DFFPE", True, False),
    "ff_alu": ("DFF", "DFF", False, False),
    "ff_alu_neg": ("DFFN", "DFFN", False, False),
    "ff_alu_reset_ce": ("DFF", "DFFRE", False, False),
    "ff_alu_remote": ("DFF", "DFF", False, False),
    "ff_alu_remote_reset_ce": ("DFF", "DFFRE", False, False),
    "ff_ram": ("DFF", "DFF", False, False),
    "ff_ram_reset_ce": ("DFF", "DFFRE", False, False),
}


class Tables(HTMLParser):
    """Read Gowin tables, including its unclosed <td> clock-frequency cell."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.tables = []
        self.table = None
        self.row = None
        self.cell = None
        self.heading = ""
        self.heading_parts = None
        self.analysis = None
        self.report_command = None

    def close_cell(self):
        if self.cell is not None and self.row is not None:
            self.row.append(" ".join("".join(self.cell).split()))
        self.cell = None

    def handle_starttag(self, tag, attrs):
        if tag in ("h1", "h2", "h3", "h4"):
            self.heading_parts = []
        elif tag == "table":
            if self.table is not None:
                raise ValueError("Nested report tables are unsupported")
            self.table = {"heading": self.heading, "analysis": self.analysis,
                          "report_command": self.report_command, "rows": []}
        elif tag == "tr" and self.table is not None:
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.close_cell()
            self.cell = []

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)
        if self.heading_parts is not None:
            self.heading_parts.append(data)

    def handle_endtag(self, tag):
        if tag in ("h1", "h2", "h3", "h4") and self.heading_parts is not None:
            self.heading = " ".join("".join(self.heading_parts).split())
            self.heading_parts = None
            match = re.search(r"report_timing\s+-(setup|hold|recovery|removal)\b", self.heading, re.I)
            if re.match(r"Report Command\s*:", self.heading, re.I):
                # Exception/pulse reports have their own numbering and
                # semantics; never inherit the last report_timing mode.
                self.analysis = match[1].lower() if match else None
                self.report_command = self.heading if match else None
        elif tag in ("td", "th"):
            self.close_cell()
        elif tag == "tr" and self.table is not None:
            self.close_cell()
            self.table["rows"].append(self.row)
            self.row = None
        elif tag == "table" and self.table is not None:
            self.tables.append(self.table)
            self.table = None


def parse_vendor(text):
    parser = Tables()
    parser.feed(text)
    metadata = {}
    paths = []
    mode = None
    current = None
    endpoints = {}
    for table in parser.tables:
        rows = table["rows"]
        # Section headings are not present on every table. Detect path summaries
        # by their content, not by path numbering or required-time timestamps.
        values = {row[0]: row[1] for row in rows if len(row) == 2}
        for key in ("Part Number", "Device Version", "Tool Version",
                    "Setup Delay Model", "Hold Delay Model"):
            if key in values:
                metadata[key] = values[key]
        mode = table["analysis"]
        if mode is None:
            current = None
            continue
        if rows and "Path Number" in rows[0] and "To Node" in rows[0]:
            for row in rows[1:]:
                if len(row) != len(rows[0]):
                    raise ValueError("Malformed vendor path-slack table")
                entry = dict(zip(rows[0], row, strict=True))
                key = (mode, table["report_command"], int(entry["Path Number"]))
                if key in endpoints and endpoints[key] != entry:
                    raise ValueError("Conflicting path endpoints within one report command")
                endpoints[key] = entry
        if "Slack" in values and "From" in values and "To" in values:
            number = re.fullmatch(r"Path\s*(\d+)", table["heading"])
            if number is None:
                raise ValueError("Timing summary without a path number")
            number = int(number[1])
            key = (mode, table["report_command"], number)
            if key not in endpoints:
                raise ValueError("Timing detail has no matching command-scoped endpoints")
            endpoint = endpoints[key]
            if any(endpoint[field].rsplit("/", 1)[0] != values[summary]
                   for field, summary in (("From Node", "From"), ("To Node", "To"))):
                raise ValueError("Timing summary and command-scoped endpoints disagree")
            current = {"analysis": mode, "number": number, "summary": values,
                       "endpoints": endpoint, "rows": []}
            paths.append(current)
        if rows and rows[0] == ["AT", "DELAY", "TYPE", "RF", "FANOUT", "LOC", "NODE"]:
            if current is None:
                raise ValueError("Timing details without a path summary")
            # Both arrival and required tables are preserved. In particular,
            # setup is a negative required-path increment, not a path timestamp.
            for row in rows[1:]:
                if len(row) != 7:
                    raise ValueError(f"Malformed timing row: {row!r}")
                current["rows"].append(dict(zip(
                    ("at_ns", "delay_ns", "type", "rf", "fanout", "location", "node"), row, strict=True)))
    for key in ("Part Number", "Device Version", "Setup Delay Model", "Hold Delay Model"):
        if key not in metadata:
            raise ValueError(f"Missing vendor metadata: {key}")
    if not paths:
        raise ValueError("No detailed vendor paths")
    arcs = []
    for path in paths:
        for row in path["rows"]:
            if row["type"].lower() not in ("tc2q", "tsu", "th", "thld", "tins", "tout", "trec", "trem"):
                continue
            delay = float(row["delay_ns"])
            if not math.isfinite(delay):
                raise ValueError("Non-finite timing increment")
            arcs.append({**row, "delay_ns": delay,
                         "analysis": path["analysis"],
                         "from": path["summary"]["From"], "to": path["summary"]["To"],
                         "from_node": path["endpoints"].get("From Node"),
                         "to_node": path["endpoints"].get("To Node")})
    return {"metadata": metadata, "paths": paths, "arcs": arcs,
            "report_commands": sorted({table["heading"] for table in parser.tables if "report_timing" in table["heading"]}),
            "reported_increment_types": sorted({row["type"] for path in paths for row in path["rows"] if row["type"]})}


def io_cell_arcs(parsed, cell_types):
    """Select single-ended I/O *cell* increments using netlist instance types.

    Locations can start IOB, IOT, IOL or IOR; location/type labels alone do not
    identify a pad arc. In particular, external input constraints can be printed
    as tC2Q on an IOBUF/IO node, and tINS also describes LUT internals. Unknown
    instance types are not inferred from names or treated as zero-delay coverage.
    This selects raw rows, not physical bounds or exhaustive RF coverage.
    """
    io_types = {"IBUF", "OBUF", "IOBUF", "TBUF"}
    return [arc for arc in parsed["arcs"]
            if arc["type"] == "tINS"
            and cell_types.get(arc["node"].rpartition("/")[0]) in io_types]


def parse_pins(text):
    parser = Tables()
    parser.feed(text)
    for table in parser.tables:
        rows = table["rows"]
        if rows and "Port Name" in rows[0] and "Loc./Bank" in rows[0]:
            pins = {}
            for row in rows[1:]:
                if len(row) != len(rows[0]):
                    raise ValueError("Malformed vendor pin row")
                pin = dict(zip(rows[0], row, strict=True))
                pins[pin["Port Name"]] = pin
            return pins
    raise ValueError("No vendor pin table")


def fixture(name):
    launch, capture, lut_data, fanout = CASES[name]
    cells = {"source": "X11Y1/DFF0", "sink": "X10Y1/DFF0", "unrelated": "X10Y1/LUT0"}
    alu_data = name.startswith("ff_alu")
    ram_data = name.startswith("ff_ram")
    local_alu = alu_data and "remote" not in name
    if ram_data:
        cells["ram"] = "X8Y1/RAM16SDP4"
    if alu_data:
        # ALU0 owns the F0 data path and reserves its paired LUT sites.
        if local_alu:
            cells["unrelated"] = "X9Y1/LUT0"
        cells["alu"] = "X10Y1/ALU0"
    body = ["module top(input clk, data, a, b, output q, lut_out);", "wire source_q;"]

    def ff(instance, kind, bel, d, q, controls=""):
        cells[instance] = bel
        attr = "keep" if local_alu and instance == "sink" else f'keep, BEL="{bel}"'
        body.append(f'(* {attr} *) {kind} {instance}'
                    f'(.CLK(clk), .D({d}), .Q({q}){controls});')

    ff("source", launch, cells["source"], "data", "source_q")
    controls = ""
    if capture.endswith("E"):
        body.append("wire ce_q;")
        ff("enable_source", launch, "X11Y1/DFF2", "a", "ce_q")
        controls += ", .CE(ce_q)"
    lsr_pin = {"DFFRE": "RESET", "DFFCE": "CLEAR", "DFFSE": "SET", "DFFPE": "PRESET"}.get(capture)
    if lsr_pin:
        body.append("wire reset_q;")
        ff("reset_source", launch, "X11Y1/DFF4", "b", "reset_q")
        controls += f", .{lsr_pin}(reset_q)"
    if alu_data:
        body.append("wire alu_d;")
        body.append('(* keep *) ALU #(.ALU_MODE(2)) alu '
                    "(.I0(source_q), .I1(a), .I3(1'b0), .CIN(1'b0), .SUM(alu_d));")
    if ram_data:
        body.append("wire [3:0] ram_do;")
        body.append("(* keep *) RAM16SDP4 #(.INIT_0(16'h1234), .INIT_1(16'h5678), "
                    ".INIT_2(16'h9abc), .INIT_3(16'hdef0)) ram "
                    "(.CLK(clk), .WRE(source_q), .WAD({source_q,a,b,data}), "
                    ".RAD({a,b,data,source_q}), .DI({source_q,a,b,data}), .DO(ram_do));")
    d_input = "ram_do[0]" if ram_data else ("alu_d" if alu_data else ("lut_d" if lut_data else "source_q"))
    ff("sink", capture, cells["sink"], d_input, "q", controls)
    if ram_data:
        init, inputs, output = "6996", ["ram_do[1]", "ram_do[2]", "ram_do[3]", "a"], "lut_out"
    elif lut_data:
        body.append("wire lut_d; assign lut_out = lut_d;")
        init, inputs, output = "6666", ["source_q", "a", "1'b0", "1'b0"], "lut_d"
    elif fanout:
        inputs = []
        for i in range(4):
            net = f"fan_q{i}"
            body.append(f"wire {net};")
            ff(f"fan_sink{i}", "DFF", f"X{12+i}Y1/DFF0", "source_q", net)
            inputs.append(net)
        init, output = "6996", "lut_out"
    else:
        init, inputs, output = "8888", ["a", "b", "1'b0", "1'b0"], "lut_out"
    ports = ", ".join(f".I{i}({signal})" for i, signal in enumerate(inputs))
    body.append(f'(* keep, BEL="{cells["unrelated"]}" *) LUT4 #(.INIT(16\'h{init})) unrelated'
                f'({ports}, .F({output}));')
    body.append("endmodule\n")
    return "\n".join(body), cells


def vendor_location(bel):
    match = re.fullmatch(r"X(\d+)Y(\d+)/(?:(DFF|LUT|ALU)(\d+)|(RAM16SDP4))", bel)
    if not match:
        raise ValueError(f"Unsupported fixture BEL: {bel}")
    x, y, _, lane, ram = match.groups()
    if ram:
        return f"R{int(y)+1}C{int(x)+1}"  # RAM16 reserves a whole logic tile
    lane = int(lane)
    return f"R{int(y)+1}C{int(x)+1}[{lane//2}][{'AB'[lane%2]}]"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(argv, cwd, env, log):
    with (cwd / "commands.jsonl").open("a") as f:
        f.write(json.dumps(argv) + "\n")
    with (cwd / log).open("w") as f:
        subprocess.run(argv, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT, check=True)


def characterize(args, name):
    out = args.out / name
    out.mkdir(parents=True, exist_ok=False)
    vendor = out / "vendor"
    vendor.mkdir()
    verilog, cells = fixture(name)
    (out / "fixture.v").write_text(verilog)
    pins = {"clk": "E2", "data": "B3", "a": "G11", "b": "D11", "q": "C3", "lut_out": "E8"}
    drive = int(name[-1]) if name.startswith("io_") else 8
    slew = "FAST" if name.startswith("io_fast") else "SLOW"
    cst = "\n".join(f'IO_LOC "{port}" {pin};\nIO_PORT "{port}" IO_TYPE=LVCMOS33 PULL_MODE=NONE'
                    + (f' DRIVE={drive} SLEW_RATE={slew}' if port in ("q", "lut_out") else "")
                    + ";" for port, pin in pins.items()) + "\n"
    (out / "fixture.cst").write_text(cst)
    clock = "create_clock -name clk -period 10.000 [get_ports {clk}]\n"
    (out / "native.sdc").write_text(clock)
    (vendor / "fixture.cst").write_text(cst + "".join(
        f'INS_LOC "{cell}" {vendor_location(bel)};\n' for cell, bel in cells.items()))
    external = "".join(f"set_input_delay -clock clk -{bound} {delay} [get_ports {{{port}}}]\n"
                       for port in ("data", "a", "b") for bound, delay in (("max", 2.0), ("min", 0.0)))
    external += "".join(f"set_output_delay -clock clk -{bound} {delay} [get_ports {{{port}}}]\n"
                        for port in ("q", "lut_out") for bound, delay in (("max", 1.0), ("min", 0.0)))
    reports = "".join(f"report_timing -{mode} -max_paths 100 -max_common_paths 8\n"
                      for mode in ("setup", "hold", "recovery", "removal"))
    (vendor / "fixture.sdc").write_text(clock + external + reports)
    env = os.environ.copy()
    env.update(args.env)
    run([args.yosys, "-Q", "-p", 'read_verilog -lib +/gowin/cells_sim.v; '
         'read_verilog fixture.v; synth_gowin -family gw5a -nolutram -nowidelut -top top '
         '-json top.json; write_verilog -noattr -noexpr vendor/netlist.v'], out, env, "yosys.log")
    command = [args.nextpnr, "--json", "top.json", "--write", "pnr.json", "--report", "native-report.json",
               "--detailed-timing-report", "--device", PART, "--vopt", "family=GW5A-25A",
               "--vopt", "cst=fixture.cst", "--vopt", "independent_ff", "--sdc", "native.sdc",
               "--seed", "1", "--timing-allow-fail", "-l", "nextpnr.log"]
    if name.startswith("ff_alu"):
        command.extend(["--vopt", "pack_alu_ff"])
    if args.chipdb:
        command.extend(["--chipdb", str(args.chipdb)])
    if args.modern_ff_timing:
        command.extend(["--vopt", "modern_ff_timing=" + str(args.modern_ff_timing)])
    run(command, out, env, "nextpnr-driver.log")
    packed = json.loads((out / "pnr.json").read_text())["modules"]["top"]["cells"]
    placements = {cell: packed[cell]["attributes"]["NEXTPNR_BEL"] for cell in cells}
    if name.startswith("ff_alu"):
        # Let native placement place the whole ALU cluster (pinning its root
        # alone leaves cluster children unbound). Freeze that actual ALU/FF
        # geometry in the vendor CST, and verify the pair, not an assumed BEL.
        paired = placements["sink"] == placements["alu"].replace("/ALU", "/DFF")
        if paired == ("remote" in name):
            raise ValueError("ALU SUM/capture FF pairing does not match the requested data mode")
        cells.update({cell: placements[cell] for cell in ("alu", "sink")})
        (vendor / "fixture.cst").write_text(cst + "".join(
            f'INS_LOC "{cell}" {vendor_location(bel)};\n' for cell, bel in cells.items()))
    if name.startswith("ff_ram"):
        cells["ram"] = placements["ram"]
        (vendor / "fixture.cst").write_text(cst + "".join(
            f'INS_LOC "{cell}" {vendor_location(bel)};\n' for cell, bel in cells.items()))
    if placements != cells:
        raise ValueError(f"Native placement mismatch: {placements}")
    result = {"case": name, "verdict": "CHARACTERIZATION_ONLY", "placements": placements,
              "io": {"standard": "LVCMOS33", "requested_output_drive_ma": drive, "requested_output_slew": slew,
                     "external_capacitive_load_pf": None, "load_provenance": "unspecified vendor default, not a board bound"},
              "constraint_difference": "Native: clock only. Vendor: same clock plus synthetic external delays and reporting options.",
              "native_report": "native-report.json", "vendor": None}
    if args.gowin:
        tcl = [f"set_device -name GW5A-25{args.device_version} {PART}",
               "add_file netlist.v", "add_file fixture.cst", "add_file fixture.sdc",
               "set_option -top_module top", "set_option -verilog_std v2001",
               "set_option -place_option 1", "set_option -route_option 1",
               "set_option -output_base_name cell_timing"]
        for pin in ("sspi", "mspi", "ready", "done", "i2c", "cpu"):
            tcl.append(f"set_option -use_{pin}_as_gpio 1")
        tcl.extend(["run syn", "run pnr"])
        (vendor / "build.tcl").write_text("\n".join(tcl) + "\n")
        env["QT_QPA_PLATFORM"] = "offscreen"
        run([args.gowin, "build.tcl"], vendor, env, "vendor.log")
        report = vendor / "impl/pnr/cell_timing_tr_content.html"
        parsed = parse_vendor(report.read_text())
        if parsed["metadata"]["Part Number"] != PART or parsed["metadata"]["Device Version"] != args.device_version:
            raise ValueError("Vendor part/version mismatch")
        # Do not accept a CST file as proof that placement was actually honored.
        located = {}
        for path in parsed["paths"]:
            for row in path["rows"]:
                instance = row["node"].rsplit("/", 1)[0]
                if instance in cells and row["location"]:
                    located.setdefault(instance, set()).add(row["location"])
        for cell in cells:
            expected = vendor_location(cells[cell])
            if located.get(cell) != {expected}:
                raise ValueError(f"Vendor placement unverified for {cell}: {located.get(cell)} != {expected}")
        parsed["verified_locations"] = {cell: sorted(locs) for cell, locs in located.items()}
        applied_pins = parse_pins((vendor / "impl/pnr/cell_timing.pin.html").read_text())
        for port, pin in pins.items():
            applied = applied_pins[port]
            if applied["Loc./Bank"].split("/")[0] != pin or applied["IO Type"] != "LVCMOS33":
                raise ValueError(f"Vendor pin/standard mismatch: {port}: {applied}")
            if port in ("q", "lut_out") and applied["Drive"] != str(drive):
                raise ValueError(f"Vendor drive mismatch: {port}: {applied}")
        parsed["verified_pins"] = applied_pins
        result["io"]["vendor_slew_status"] = (
            "IGNORED_BY_TOOL" if "ignore SlewRate setting" in (vendor / "vendor.log").read_text() else "NOT_CONFIRMED")
        (out / "vendor-arcs.json").write_text(json.dumps(parsed, indent=2) + "\n")
        result["vendor"] = {"report_sha256": sha(report), "metadata": parsed["metadata"],
                            "path_count": len(parsed["paths"]), "arc_count": len(parsed["arcs"])}
    result["input_sha256"] = {str(p.relative_to(out)): sha(p) for p in (
        out / "fixture.v", out / "fixture.cst", out / "native.sdc", out / "top.json",
        vendor / "netlist.v", vendor / "fixture.cst", vendor / "fixture.sdc")}
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--yosys", default=os.environ.get("YOSYS", "yosys"))
    parser.add_argument("--nextpnr", default=os.environ.get("NEXTPNR_HIMBAECHEL", "nextpnr-himbaechel"))
    parser.add_argument("--gowin", default=os.environ.get("GOWIN_SH"), help="optional vendor executable/wrapper")
    parser.add_argument("--chipdb", type=Path)
    parser.add_argument("--modern-ff-timing", type=Path, help="explicit experimental FF sidecar; legacy by default")
    parser.add_argument("--device-version", choices=("A", "B"), default="B", help="vendor device revision, retained in report metadata")
    parser.add_argument("--env-json", type=Path, help="explicit tool environment overrides (values are not written to reports)")
    args = parser.parse_args()
    args.out = args.out.resolve()
    if args.out.exists() and any(args.out.iterdir()):
        parser.error("Output directory must be fresh/empty; existing evidence will not be overwritten")
    args.env = json.loads(args.env_json.read_text()) if args.env_json else {}
    if args.chipdb:
        args.chipdb = args.chipdb.resolve(strict=True)
    if args.modern_ff_timing:
        args.modern_ff_timing = args.modern_ff_timing.resolve(strict=True)
    for name in ("yosys", "nextpnr", "gowin"):
        value = getattr(args, name)
        if value:
            executable = shutil.which(value, path=args.env.get("PATH", os.environ.get("PATH")))
            if not executable:
                parser.error(f"Cannot find {name}: {value}")
            setattr(args, name, str(Path(executable).absolute()))
    args.out.mkdir(parents=True, exist_ok=True)
    tools = {name: {"path": getattr(args, name), "sha256": sha(getattr(args, name))}
             for name in ("yosys", "nextpnr", "gowin") if getattr(args, name)}
    manifest = {"part": PART, "vendor_device_version_requested": args.device_version,
                "seed": 1, "period_ns": 10.0, "tools_at_command_paths": tools,
                "runner_sha256": sha(__file__),
                "explicit_chipdb_sha256": sha(args.chipdb) if args.chipdb else None,
                "modern_ff_sidecar_sha256": sha(args.modern_ff_timing) if args.modern_ff_timing else None,
                "verdict": "CHARACTERIZATION_ONLY"}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    results = []
    for name in args.case or CASES:
        print(f"Characterizing {name} (not sign-off)", flush=True)
        results.append(characterize(args, name))
        (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"Collected {len(results)} fixtures; CHARACTERIZATION_ONLY", flush=True)


if __name__ == "__main__":
    main()
