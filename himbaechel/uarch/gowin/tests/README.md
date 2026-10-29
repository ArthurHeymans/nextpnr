# Focused Gowin regressions and timing characterization

With a GW5A-25A nextpnr build and Yosys in PATH:

```sh
python3 himbaechel/uarch/gowin/tests/alu_ff.py
python3 himbaechel/uarch/gowin/tests/independent_ff.py
python3 himbaechel/uarch/gowin/tests/bram_timing.py
python3 himbaechel/uarch/gowin/tests/cell_timing_test.py
```

`YOSYS` and `NEXTPNR_HIMBAECHEL` override the executable paths. The independent
FF cases pin a LUT/register pair, check opt-in behavior, keep constant FFs on
the LUT path, reject conflicting wide-LUT select nets, and permit a shared
select net. Successful cases check the route of the actual FF's D net. The
BRAM test checks both address ports on all dual-port timing variants and
rejects erroneous CLKB arcs on single-port variants.

To compare STA with vendor tools installed:

```sh
bash himbaechel/uarch/gowin/tests/ff_d_timing.sh /tmp/gowin-ff-timing
```

`GOWIN_SH` may name a wrapper for `gw_sh`. The fixture fixes source DFF0 at
R2C12 and sink DFF0 plus an unrelated LUT at R2C11. It exports the same primitive
netlist to the vendor tools and preserves the locations, but does **not** fix
the routing or establish that the two tools use identical PVT characterization.
Inspect the vendor report to verify that its source/sink locations were honored.

Read Fmax from the vendor frequency summary and slack/data delay from its setup
table. `.timing_paths` starts each SETUP record with slack, arrival timestamp,
and required timestamp. It can also include clock-tree cells before the launch
register; do not infer a launch node from the first cell in that list.

This is not a hardware-frequency test, a general timing-model calibration, or
phase-aware I/O sign-off. Do not multiply all device delays by a ratio measured
on one short register path.

## Bounded FF and I/O characterization matrix

```sh
python3 himbaechel/uarch/gowin/tests/cell_timing.py \
    --out /tmp/gw5a-cell-timing \
    --chipdb /path/to/chipdb-GW5A-25A.bin \
    --gowin /path/to/gw_sh-wrapper
```

The output directory must be fresh/empty. `--case ff` restricts the matrix;
`--device-version A` requests the vendor revision-A control (default B).
`--env-json` optionally supplies a frozen tool environment. Without `--gowin`
(or `GOWIN_SH`), only native characterization is run. The vendor wrapper must
accept a Tcl filename and provide any required library paths/headless setup.
The same Yosys primitive netlist is exported to both backends. Actual source,
sink, control-source and companion/data-LUT locations are verified, as are
vendor pad locations, I/O standard and output drive. Routes are **not** fixed
or equivalent between tools. Manifests hash command-path executables, explicit
chipdb inputs, runner and designs; record a wrapper's underlying tools too.

The original twelve cases cover positive/negative-edge FFs, CE, synchronous reset,
asynchronous clear, LUT-fed data, fanout, and four requested LVCMOS33
4/8 mA × SLOW/FAST combinations. Native constraints contain only a 100 MHz
base clock. Vendor constraints additionally use synthetic input max/min
2/0 ns and output max/min 1/0 ns to expose I/O paths. These are probes, **not
NORbert board constraints**, nor an equivalent whole-path STA comparison.

The parser retains actual part/revision and setup/hold model labels, endpoint
pins, RF branches, signed required-path increments and original tables. It
never calls a timestamp a cell delay, an ignored setting an applied one, or
missing timing coverage zero. Return code zero means evidence collection
completed, **not** that timing or hardware passed. `cell_timing_test.py` runs
without vendor tools and is included in CI.

### Observed GW5A-25 C1/I0 discrepancies

A frozen 12-case matrix plus a revision-A control found the following reported
cell values (ns). Vendor defaults label setup **Slow 0.855V 0C C1/I0** and hold
**Fast 0.945V 85C C1/I0**; I/O reports say LVCMOS33/3.3V bank supply.

| Quantity | Native model | Vendor observed |
|---|---:|---:|
| Positive-edge clock-to-Q, setup paths | 0.231 | 0.368 RF / 0.382 RR |
| Negative-edge clock-to-Q, setup paths | 0.231 | 0.442 FF |
| D setup requirement, independent FF data | 0.035 | 0.291 positive / 0.236 negative |
| D setup requirement, local LUT-fed data | 0.035 | 0.064 |
| Positive CE / synchronous RESET setup | 0.035 / 0.035 | 0.311 / 0.373 |
| Input/output pad arcs | absent | e.g. 0.683 / 2.556 on reported RR setup branches |

These are **observed branches**, not exhaustive transition/corner bounds or
calibrated primitive constants. LUT-fed and independent paths also differ in
their reported data transition, so the setup difference is not an isolated
measurement of a SEL mux delay. Native hold entries and signed vendor hold
increments likewise cannot be repaired by scaling all delays. Recovery/removal
reports must retain their analysis type even when their row types are named
`tSu`/`tHld`. Revision A/B returned identical arcs in the control fixture;
this does not establish general device interchangeability.

Important coverage findings:

- This vendor version explicitly logs **ignore SlewRate setting**. The requested
  slew matrix is not a physical slew characterization. Drive4/8 is verified in
  pin reports, but the observed pad-delay increments do not change with it;
  that is not evidence that real drive strength has no timing effect.
- External capacitive load remains an unspecified tool default, not a board
  bound. The C3 output fixture is not a forwarded-SDRAM-clock pad bound.
- HTML reports confirm `-max_paths 100 -max_common_paths 8`. These enumerate
  paths, not every RF combination for a given path: only the selected reported
  branches are claimed, and unreported transitions remain unknown.
- Operating-condition application probes change I/O increments (e.g. output
  2.556→2.536 ns for a fast-setup request), but observed FF/LUT increments do
  not follow the same request. This remains true when the request precedes
  reporting commands. A common, fully validated PVT interpretation is unresolved.
- The frozen database's FF group matches the legacy prefix read from **both**
  installed vendor revision timing files. The GW5A-25A reader parses three
  15,552-byte legacy chunks of a 2,275,269-byte file; the rest is uninterpreted
  by that reader. This does not prove the contents or purpose of the tail.
- The generator collapses four legacy entries to min/max, uses primitive FF
  type/clock edge but no data-route variant, and emits no IBUF/OBUF timing
  variant. Missing I/O arcs are omitted coverage, not zero physical delay.

No timing constants, default recipes, chipdb or release pins were changed from
these observations. Correcting the model requires authoritative mapping of
source data, route/transition distinctions and operating corners—not fitting
one fixture's Fmax. Full evidence for this run is retained locally under
`/tmp/norbert-cell-timing-K1dtuX/RESULTS.md`.

### Timing-file provenance follow-up

A subsequent offline audit locates the active FF data in an appended common-
model block, not the legacy DFF quartet consumed by the current chipdb flow.
Apicula's opt-in `python -m apycula.tm_inspect FILE.tm` now exposes these raw
records and their exact byte offsets without changing chipdb generation.
For the inspected 25A/B files, the block occupies bytes46656–62162, has nine
models and includes distinct DI_M/DI_FS/DI_OFX, clock-polarity and corner records.
The first15 bytes are an opaque, differing A/B preamble—not a universal magic.

Vendor code's device-dependent1.25 slow-group multiplier, together with those
records, accounts for the default-profile reported FF increments. Read-only
kernel file overrides causally distinguish slow versus fast groups, M versus
FS inputs and positive versus negative clock-to-Q. This is not permission to
multiply legacy timings by1.25 or to apply the profile to other devices/grades.
The inspector deliberately does **not** normalize values or enable new timing
models. See Apicula `doc/norbert-database.md` for the source mapping and limits;
full traces/controls are at `/tmp/norbert-tm-provenance-EMh6CR/RESULTS.md`.

## Opt-in modern FF timing sidecar

Using the matching working-tree Apicula, export a **new** file from one of the
witnessed V1.9.11.03 25A/B `.tm` files (inputs are not distributed):

```sh
python -m apycula.gw5a_ff_timing /path/to/GW5A-25A.tm \
    --profile GW5A25-V1.9.11.03-default-C1/I0 --output /tmp/ff-timing.json
nextpnr-himbaechel ... --vopt modern_ff_timing=/tmp/ff-timing.json
# Optional native matrix; omit --gowin/GOWIN_SH to avoid invoking vendor tools:
python3 himbaechel/uarch/gowin/tests/cell_timing.py \
    --out /tmp/modern-ff-matrix --modern-ff-timing /tmp/ff-timing.json
```

The ordinary flow remains **legacy by default**. The sidecar overrides only
fabric FF clocking queries; it neither regenerates nor modifies the chipdb.
The producer checks the complete witnessed source hash and explicit profile;
the consumer checks schema, declared source identity, record completeness,
integer ranges and exact target `GW5A-LV25MG121NC1/I0`/`GW5A-25A`. Metadata is
provenance, not authentication: preserve/hash the generated sidecar too.
Both sources have identical witnessed common records; this does not claim
whole-device A/B interchangeability or cached chipdb provenance.

Setup/recovery uses common group1 ×1.25; signed hold/removal uses group2 ×1.
All four slots are enveloped rather than assigning unvalidated RF/unateness
indices. Constraints are rounded outward (floor minimum/ceil maximum) to ps;
clock-Q spans fast-group minimum through scaled slow-group maximum, with
separate positive/negative clock records. These are **source-slot envelopes**,
not exhaustive physical PVT bounds. Negative hold is preserved, not negated or
clamped; positive hold remains positive. Recovery/removal uses nextpnr's
setup/hold API and is not a complete asynchronous reset/glitch analysis.

Before routing, D uses the envelope of M and FS. A routed D input's **actual
last pip** selects SEL→XD (`DI_M`) or F→XD (`DI_FS`). Every FF must have a
supported routed input at post-route validation; no unknown route silently
gets a zero or legacy arc. Supported types are the fixture-covered DFF, DFFN,
DFFE, DFFNE, DFFRE, DFFCE, DFFSE and DFFPE. SET shares the synchronous LSR
record; PRESET shares the asynchronous recovery/removal record. Additional
paired fixtures validate local ALU SUM→F→XD as FS (positive/negative clocks and
reset/CE), remote ALU SUM→SEL→XD as M, and RAM16SDP4 DO→SEL→XD as M (DO[0]
exercised). RAM output bus indices are expanded frontend pins, not extra FF
input modes. ALU carry, RAM16-through-local-F, OFX/wide-mux sources, other FF
types, latches and requested I/O-register packing still fail explicitly. These are fabric FF arcs, not a
complete NORbert/device timing model.
Routing-time cached clocking information may retain the conservative unrouted
envelope; the final fresh timing analysis queries actual routes.

Settings record source hash, sidecar path and experimental verdict; routed FFs
carry `MODERN_FF_DATA_MODE`. The matrix manifest additionally hashes the sidecar.
No IBUF/OBUF/IOBUF arcs, board/pad/load bounds, generated-clock phase STA,
other profiles or operating-condition overrides are supplied. Other cell and
routing timing stays as in the supplied chipdb. MHz is a model estimate, not
measured operating frequency or sign-off. Defaults, build recipes and the
calibrated NORbert image are unchanged.

Synthetic normalization tests live in Apicula `tests/test_gw5a_ff_timing.py`;
native `ModernFFTest` tests the consumer, signed constraints, edge selection,
unrouted envelopes, routed mux selection, source-specific guards and unsupported
coverage. No vendor software or timing input is needed for those tests.

Eleven further cases (`ff_set_ce`, `ff_preset_ce`, their `_lut` variants,
`ff_alu`, `ff_alu_neg`, `ff_alu_reset_ce`, `ff_alu_remote`,
`ff_alu_remote_reset_ce`, `ff_ram`, `ff_ram_reset_ce`) bring the matrix to23.
ALU and RAM clusters must be placed as complete clusters: those cases let native
placement choose the cluster (and paired ALU FF), then freeze
its **observed** geometry in the vendor CST. Other FF/LUT/pad sites remain
fixed a priori. Both backends' actual placements are verified; no claim is
made that unconstrained cluster geometry stays identical across timing models.
RAM placement reports identify the whole tile, not a fictional LUT lane.
Matched primitive data/controls and the actual FF input pip—not scalar Fmax—
select the compared source records. Only selected reported RF branches are
observed; unreported transitions and physical bounds remain unknown.

### Bounded NORbert hardware follow-up

A fresh model-enabled layout was tested with the programmed 120 MHz PLL
configuration. On one unchanged placement/route, capture coarse5 failed twice
(33/29 mismatching byte comparisons; DQ0/DQ1), while coarse6/7/8 each had zero
errors in655,360 byte comparisons. The known-good control was clean. Only the
capture PLL coarse parameter changed between candidate images. This indicates
read/capture phase sensitivity, not a complete capture window or unique
localization of a cell violation; aux→system timing also changes with phase.

The new layout's system estimate remains119.92 MHz against120 MHz (**FAIL**).
Functional operation in these conditions is not a measured Fmax, proof of model
accuracy, phase/I/O/PVT closure or permission to fit delay constants. The trial
used UART/SDRAM reads/writes with SPI stopped, not external SPI transactions.
The known-good volatile reference and the entire affected32 MiB backup were
restored byte-exact, with final SPI STOPPED; no persistent flash/EEPROM writes.
Defaults and the previous calibrated disk image remain unchanged. Full
commands, failures/repairs, image hashes, tests and restoration evidence:
`/tmp/norbert-modern-hw-tjLTFA/RESULTS.md`.

### Offline I/O provenance follow-up (no timing enablement)

Apicula's separate `python -m apycula.tm_io_inspect FILE.tm` inspects checked
legacy pad records and two ODDR clock-Q slices. It does not supply native
IBUF/OBUF/IOBUF timing, change the FF sidecar or enable I/O-register packing.
For the audited25B default-profile runs (matching A/B raw I/O records),
disassembly and read-only kernel overlays identify legacy input,
fabric/IOLOGIC data-output and OE-output records.
Data-output getters add a coded2 ns; OE getters do not. ODDR clock-Q uses
separate legacy positive/negative quartets, **not the modern fabric FF model**.
No FF1.25 scale or physical/PVT bound may be inferred for these I/O fields.

`io_cell_arcs(parsed, cell_types)` selects raw single-ended I/O cell increments
using actual netlist instance types, not location prefixes: IOT/IOL/IOR pads
matter too. LUT internals are also `tINS`; external IOBUF input constraints
can be printed as `tC2Q`, and output constraints as `tOut`. Neither a numeric
row nor its TYPE/LOC alone identifies a physical pad arc. Unknown types are
not guessed into coverage. The parser's default output and23-case native
fixture set are unchanged; synthetic tests cover this additional selector.

Vendor `set_load`/`set_input_transition` requests were rejected, while SLEW
requests emit ignore notices. Applied DRIVE/IO-standard attributes and unchanged
selected pad increments do not establish electrical invariance. Ordinary
forwarded-output paths disappear when the pad is declared a generated-clock
target; missing paths are not zero clock-to-pad latency. Actual electrical,
board-flight, propagated-clock, OE-turnaround and PVT bounds remain unknown.
See Apicula `doc/norbert-database.md` and the complete offline evidence at
`/tmp/norbert-io-provenance-7JkP3I/RESULTS.md`. No hardware operations, installed
resource edits, database regeneration, default recipe changes or timing sign-off.

### Scoped report identity and generated-clock controls

A subsequent offline NORbert audit exposed repeated path numbers across distinct
`report_timing` commands of the same mode. The parser now keys endpoint tables
by **analysis + complete command heading + path number**, checks instance
identity against detailed summaries, and rejects missing/conflicting matches.
It clears timing context for exception/pulse commands rather than relabeling
those details as the previous timing mode. This fixture parser does not claim
exception/pulse coverage. Six synthetic tests exercise these boundaries;
all28 prior collected I/O-fixture parses remain exactly unchanged.

On six independent vendor NORbert controls, changing the forwarded generated
clock's explicit **source** early/late latency has the expected four setup/hold
signs; a network-latency request did not shift those paths. Port/net/PLL-pin
clock targets gave equal observed reports. All six encoded configuration
payloads were identical (only creation-time comments differed), so these were
fixed-vendor-configuration application controls, not merely equal scalar Fmax.
Two further max/min external-delay controls verify input/output setup/hold
application. All eight binary configurations are byte-identical. They are still
**not STA of native geometry or physical latency bounds**. Source latency is
folded into external `tC2Q`/`tOut` constraint increments while the named clock
row stays zero; that row alone does not prove the request was ignored. Never
classify those combined increments as physical cell delays or add latency twice.

Native PLL-output-to-OBUF-input routes and53 register/pin paths were separately
root-verified against the frozen nextpnr geometry. Pad, PLL/electrical and board
terms remain explicit unknowns in conditional accounting; no I/O arcs were
enabled. Full-VCO phases and signed hold/output minima are preserved. Evidence:
`/tmp/norbert-clock-propagation-2OdaUD/RESULTS.md`.

### Five-seed model comparison

With one frozen NORbert mapping and otherwise identical backend settings,
legacy estimates meet120 MHz in5/5 seeds (worst/median120.03/123.56 MHz),
while modern-FF estimates miss in5/5 (113.79/116.50 MHz). Resources are identical:
3001 logic LUTs,466 ALUs,2520 fabric FFs,32 RAM16 plus128 shadow LUTs,4 BSRAMs.
Different-model MHz are not commensurate placement-quality scores, measured
Fmax or a reason to revert the source-backed timing model to claim closure.

That frozen build's packed-JSON timing-only import aborted before analysis,
so that experiment claimed no full cross-retimed Fmax. Re-costing only its
observed critical paths under common modern FF costs put legacy seeds2–5 over
the120 MHz period; seed1 remained unresolved at that stage. Modern
reported paths self-reproduce under that calculation. Full source/mux checks
and530 pin/register route diagnostics have no unsupported coverage, but their
source-model balances still lack physical PLL/pad/electrical/board/PVT bounds.
No I/O enablement, recipe change, bitstream generation or hardware test:
`/tmp/norbert-five-seed-yLNKfg/RESULTS.md`.

### Verified timing-only checkpoint reload

`--timing-only` is an opt-in **router-source-model reanalysis**, currently limited
to `GW5A-LV25MG121NC1/I0` / `GW5A-25A` and the audited identity-pin-map primitive
subset. It requires completed synth/pack/place/route checkpoints, every cell's
BEL, complete saved route triples, and driver-root-verified wires/sinks. It never
calls packing, placement, global routing, post-placement transforms or postRoute.
Unsupported DSP/IOLOGIC/latch/pin-map cases and unnamed/dummy bus bits reject,
rather than being approximately reconstructed.

For example, with an existing output directory and **new** output filenames:

```sh
nextpnr-himbaechel --timing-only --no-pack --no-place --no-route \
  --device GW5A-LV25MG121NC1/I0 --chipdb CHIPDB.bin \
  --json SAVED-PNR.json --sdc PACKED-CLOCKS.sdc \
  --vopt family=GW5A-25A --vopt independent_ff --vopt pack_alu_ff \
  --vopt modern_ff_timing=FF-SIDECAR.json \
  --timing-allow-fail --detailed-timing-report \
  --write NEW/reloaded.json --report NEW/report.json
python3 himbaechel/uarch/gowin/tests/timing_reload.py \
  SAVED-PNR.json NEW/reloaded.json --report NEW/report.json \
  --reference-report ORIGINAL-SAME-MODEL-report.json --output NEW/self-control.json
```

Omit the sidecar option to select legacy chipdb FF timing; inherited modern-model
settings are then cleared. Active model/file/source provenance is refreshed from
the actual consumer, not trusted from the saved JSON. Cell parameters and attrs
are not rewritten. Net aliases, unused nets, top-level ports and disconnected
scalar cell-port descriptors are preserved. The checker permits only signal-ID
renumbering and route-triple ordering differences, not geometry/connectivity,
parameter, attribute, wire, pip or strength changes. Settings identify the new
analysis and are deliberately separate from that geometry fingerprint.

Clock constraints are not serialized. Supply an explicit SDC on the **packed
clock nets**; input-buffer/PLL constraint propagation is not rerun. An originally
unconstrained clock requires its exact name with
`--timing-only-unconstrained-clock NAME`; missing/unused declarations reject.
This retains absence and the checkpoint's native target-frequency fallback,
not a newly justified clock waveform. The NORbert controls explicitly retain
`spi_trx_i.spi_io0_out_DFFRE_Q_CLK` unconstrained (native12 MHz fallback).
PAD-side nets are not fabric routes. A retained BUFG input-marker net is accepted
only in the exact global-router residual structure, with its physical input wire
witnessed on the fully driver-root-verified real net; it is not a blanket exception
for missing routes. Existing outputs, input/output aliases, force, script hooks,
GUI, ignored loops/clock relations, frequency overrides and SDF output reject.

**Delay-mode provenance matters:** `Arch::route()` reports while fast-pip delays
are enabled. Reanalysis explicitly uses that same mode and records
`HIMBAECHEL_ROUTER_FAST_PIP_DELAYS`; it does not qualify full RC reconstruction.
Ordinary JSON binding accumulates RC state in serialization order and is not a
substitute. The initial full-RC probe did not self-reproduce and is excluded.
This is a source-code-backed mode selection, not delay fitting. Merely requesting
`--report` without explicit timing analysis still does not perform reanalysis.

The offline follow-up first reproduced **all report fields** for all ten original
layouts under their own models (order normalization,1e-6 absolute tolerance),
including detailed increments, not just rounded MHz. All cells/nets/ports/params
and route geometry are preserved. Two fresh full-design seed1 default-flow
controls produce byte-identical routed JSON and native reports. After the ten
original-model self-controls, all ten frozen layouts were reanalysed with one
common modern FF + router-fast-pip model:

| Layout seed | Legacy-placed, common modern MHz | Modern-placed, common modern MHz |
|---|---:|---:|
|1|128.88|119.92|
|2|116.93|113.79|
|3|117.62|115.05|
|4|114.06|116.50|
|5|118.48|118.55|

Worst/median/best: legacy-placed114.06/117.62/128.88 MHz,
modern-placed113.79/116.50/119.92 MHz. Spreads14.82/6.13 MHz;1/5 versus0/5 meet
the frequency-only120 MHz target. The former seed1 uncertainty is resolved only
for this source model, not physical timing. No resource savings, recipe/model
promotion, I/O timing enablement, bitstreams or hardware acceptance follow.
PLL phase/external/hold/CDC/electrical/board/PVT limitations remain.

`timing_reload_test.py` tests the preservation/report checker.
`timing_reload_flow.py` runs two tiny native fixtures, exact self-controls and
fail-closed cases in CI; its invented sidecar numbers are **synthetic only**, not
new device characterization or programming inputs. Full evidence, including
failed intermediate controls and build provenance:
`/tmp/norbert-timing-reload-7ryD95/RESULTS.md`.

### Placement timing trace and bounded heuristic follow-up

Gowin exposes an opt-in HeAP diagnostic:

```sh
# Add to an otherwise unchanged native placement command; FILE must be new.
--vopt placer_timing_trace=NEW-DIRECTORY/heap.jsonl
python3 himbaechel/uarch/gowin/tests/placer_timing_trace.py --check NEW-DIRECTORY/heap.jsonl
```

JSONL records unplaced setup, legal iterations, the minimum-HPWL retained
solution and post-refinement estimates: sink/driver identity and BELs,
criticality, predicted route/setup slack, queried setup/signed hold and predicted
clock scores. Missing estimates remain null. HPWL is numeric only for the legal
objective/retained solution; unplaced/post-refinement HPWL is null because the
refiner does not refresh HeAP's private coordinate cache. These are
**placement-heuristic estimates**, not routed or phase-aware STA, physical bounds
or acceptance.
HeAP constructs its cell-arc cache before placement; queried constraints are
labelled as queries, not a claim to rebuild that cache. Tracing requires
timing-driven placement, rejects existing files/symlinks, and must be requested
by the current CLI rather than inherited from a checkpoint. The default path
neither interns diagnostic setting IDs nor changes HPWL selection/convergence.

The five-seed offline follow-up held mapping, resources and final active model
fixed. Every candidate preserved exact geometry through frozen-v8 common-modern
reanalysis and reproduced every serialized native report field. All30 candidate
routes are retained, including the initial collector failure and independently
reverified original files. No timing values were fitted or edited.

| Placement variant | Worst / median / best MHz | System target count |
|---|---:|---:|
| Modern baseline: exponent2, weight60 |113.79 /116.50 /119.92|0/5|
| Retain best predicted-timing legal iteration |110.10 /113.05 /115.42|0/5|
| Fixed packed-cluster FS setup hint |110.55 /115.39 /120.38|1/5|
| Exponent4, weight60 |112.49 /121.08 /121.51|3/5|
| Exponent2, weight30 |111.22 /116.36 /120.48|1/5|
| Exponent3, weight60 |112.74 /118.09 /123.08|1/5|
| Exponent4, weight30 |105.76 /116.62 /122.32|1/5|

**No robust improvement:** every candidate regresses the baseline worst seed.
Exponent4 improves median/target count, not worst-seed reliability. Five seeds
are not population-level superiority evidence. All have3001 logic LUTs,
466 ALUs,2520 FFs,32 RAM16s plus128 shadow LUTs and4 BSRAMs. Coordinate footprint
and routing vary; all eight RAM lanes count, and none is a resource/area win.

The trace confirms conservative unrouted M/FS setup292ps on positive-edge
paired FS D inputs, versus final FS64ps (negative-edge records stay separate),
alongside genuine CE319ps/synchronous-reset475ps costs.
The experimental fixed-cluster hint covered1198 packer-guaranteed FS paths,
validated against actual final original pips in every seed;15 later passthroughs
were deliberately excluded. Correcting this alone did not improve worst/median.
Likewise a better predicted legal-iteration score did not yield better routed
results. Criticality normalization, coarse route prediction, refinement and
congestion remain interacting hypotheses, not proof the source model is wrong.

The two unsuccessful code-level heuristics and their21-test experimental build
are archived privately; neither option nor altered FF timing remains in public
source. Only diagnostic tracing is retained. Final native19 + Python147 units,
packing/BRAM/reload and the new trace CI controls pass. Final legacy/modern seed1
default PNR/reports are byte-identical; trace-only controls preserve geometry
and all report fields. No recipe/model promotion, bitstream generation, hardware
access or new physical acceptance. Evidence:
`/tmp/norbert-placer-timing-q0Ruvh/RESULTS.md`.
