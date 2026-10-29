#!/usr/bin/env bash
# Compare STA on matching register/LUT locations. Routes may differ; this is
# characterization evidence, not a full timing-model calibration or sign-off.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "${1:?usage: ff_d_timing.sh OUTPUT_DIRECTORY}"
out=$(cd "$1" && pwd)
mkdir -p "$out/vendor"
yosys=${YOSYS:-yosys}
nextpnr=${NEXTPNR_HIMBAECHEL:-nextpnr-himbaechel}
gowin=${GOWIN_SH:-gw_sh}

"$yosys" -Q -q -p "read_verilog -lib +/gowin/cells_sim.v; read_verilog \"$here/ff_d_timing.v\"; synth_gowin -family gw5a -nolutram -nowidelut -top top -json \"$out/top.json\"; write_verilog -noattr -noexpr \"$out/vendor/netlist.v\""
cp "$here/ff_d_timing.cst" "$out/vendor/timing.cst"
printf 'INS_LOC "source" R2C12[0][A];\nINS_LOC "sink" R2C11[0][A];\nINS_LOC "unrelated" R2C11[0][A];\n' >> "$out/vendor/timing.cst"
printf 'create_clock -name clk -period 10.000 [get_ports {clk}]\n' > "$out/vendor/timing.sdc"
"$nextpnr" --json "$out/top.json" --write "$out/pnr.json" \
    --device GW5A-LV25MG121NC1/I0 --vopt family=GW5A-25A \
    --vopt "cst=$here/ff_d_timing.cst" --vopt independent_ff \
    --sdc "$out/vendor/timing.sdc" --timing-allow-fail -l "$out/nextpnr.log"

printf '%s\n' \
    'set_device -name GW5A-25B GW5A-LV25MG121NC1/I0' \
    'add_file netlist.v' 'add_file timing.cst' 'add_file timing.sdc' \
    'set_option -top_module top' 'set_option -verilog_std v2001' \
    'set_option -use_sspi_as_gpio 1' 'set_option -use_mspi_as_gpio 1' \
    'set_option -use_ready_as_gpio 1' 'set_option -use_done_as_gpio 1' \
    'set_option -use_i2c_as_gpio 1' 'set_option -use_cpu_as_gpio 1' \
    'set_option -place_option 1' 'set_option -route_option 1' \
    'set_option -output_base_name ff_d_timing' 'run syn' 'run pnr' > "$out/vendor/build.tcl"
(cd "$out/vendor" && "$gowin" build.tcl > vendor.log 2>&1)
printf 'Reports retained in %s; compare slack/data delay, not required-time timestamps.\n' "$out"
