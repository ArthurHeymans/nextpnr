#include "timing.h"
#include "command.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
#define HIMBAECHEL_CONSTIDS "uarch/gowin/constids.inc"
#include "himbaechel_constids.h"
#include "../modern_ff.h"

NEXTPNR_NAMESPACE_BEGIN

TEST(GowinCheckpointTest, BindingBeforePrePlaceKeepsThePlacementCache)
{
    init_share_dirname();
    ArchArgs args;
    args.device = "GW5A-LV25MG121NC1/I0";
    auto ctx = std::make_unique<Context>(args);
    ctx->uarch->init(ctx.get());
    ctx->late_init();
    auto *lut = ctx->createCell(ctx->id("lut"), id_LUT4);
    auto *ff = ctx->createCell(ctx->id("ff"), id_DFF);
    lut->addOutput(id_F);
    ff->addInput(id_D);
    lut->connectPort(id_F, ctx->createNet(ctx->id("lut_f")));
    ff->connectPort(id_D, ctx->createNet(ctx->id("unrelated_data")));
    auto lut_bel = ctx->getBelByNameStr("X11Y1/LUT0");
    auto ff_bel = ctx->getBelByNameStr("X11Y1/DFF0");
    ASSERT_NE(lut_bel, BelId());
    ASSERT_NE(ff_bel, BelId());
    // JSON restoration does this before prePlace() has ever run.
    ctx->bindBel(lut_bel, lut, STRENGTH_LOCKED);
    ctx->bindBel(ff_bel, ff, STRENGTH_LOCKED);
    ctx->uarch->prePlace();
    // Default packing forbids unrelated LUT/FF sharing. A reset of the cache
    // in prePlace() would lose both bindings and incorrectly return true.
    EXPECT_FALSE(ctx->isBelLocationValid(ff_bel));
    ctx->unbindBel(ff_bel);
    ctx->unbindBel(lut_bel);
}

struct EdgePairTiming
{
    double fmax = 0;
    double slack_ns = 0;
};

// Build a single register-to-register path on one clock net, with the given
// waveform, and analyse it. The route delay of the data net is path_ns, and the
// launch and capture registers can be skewed apart by clock route delays.
static EdgePairTiming analyse_edge_pair(IdString src_type, IdString dst_type, double high_ns, double low_ns,
                                        double path_ns, double launch_skew_ns = 0, double capture_skew_ns = 0)
{
    init_share_dirname();
    ArchArgs args;
    args.device = "GW5A-LV25MG121NC1/I0";
    auto ctx = std::make_unique<Context>(args);
    ctx->uarch->init(ctx.get());
    ctx->late_init();
    ctx->settings[ctx->id("target_freq")] = Property(40000000);

    auto *clk = ctx->createNet(ctx->id("clk"));
    clk->clkconstr = std::make_unique<ClockConstraint>();
    clk->clkconstr->period = DelayPair(ctx->getDelayFromNS(high_ns + low_ns));
    clk->clkconstr->high = DelayPair(ctx->getDelayFromNS(high_ns));
    clk->clkconstr->low = DelayPair(ctx->getDelayFromNS(low_ns));
    auto *data = ctx->createNet(ctx->id("data"));

    auto *src = ctx->createCell(ctx->id("src"), src_type);
    src->addInput(id_CLK);
    src->addOutput(id_Q);
    src->connectPort(id_CLK, clk);
    src->connectPort(id_Q, data);
    auto *dst = ctx->createCell(ctx->id("dst"), dst_type);
    dst->addInput(id_CLK);
    dst->addInput(id_D);
    dst->connectPort(id_CLK, clk);
    dst->connectPort(id_D, data);
    ctx->assignArchInfo();

    TimingAnalyser timing(ctx.get());
    timing.with_clock_skew = true;
    timing.setup(false, false, true);
    timing.set_route_delay(CellPortKey(dst->name, id_D), DelayPair(ctx->getDelayFromNS(path_ns)));
    timing.set_route_delay(CellPortKey(src->name, id_CLK), DelayPair(ctx->getDelayFromNS(launch_skew_ns)));
    timing.set_route_delay(CellPortKey(dst->name, id_CLK), DelayPair(ctx->getDelayFromNS(capture_skew_ns)));
    timing.run(false, false, false, true);

    EdgePairTiming result;
    result.fmax = timing.get_timing_result().clock_fmax.at(clk->name).achieved;
    result.slack_ns = ctx->getDelayNS(timing.get_setup_slack(CellPortKey(dst->name, id_D)));
    return result;
}

TEST(TimingSkewTest, OppositeEdgesShareTheClockTree)
{
    auto baseline = analyse_edge_pair(id_DFF, id_DFFN, 12.5, 12.5, 1);

    auto skewed = analyse_edge_pair(id_DFF, id_DFFN, 12.5, 12.5, 1, 2);
    EXPECT_NEAR(skewed.fmax, 500.0 / (500.0 / baseline.fmax + 2.0), 0.01);

    auto partially_cancelled = analyse_edge_pair(id_DFF, id_DFFN, 12.5, 12.5, 1, 2, 1);
    EXPECT_NEAR(partially_cancelled.fmax, 500.0 / (500.0 / baseline.fmax + 1.0), 0.01);
}

TEST(TimingSkewTest, OppositeEdgesUseTheClockHighTime)
{
    // Rising launch, falling capture: only the clock's high time is available.
    auto even = analyse_edge_pair(id_DFF, id_DFFN, 12.5, 12.5, 1);
    auto duty = analyse_edge_pair(id_DFF, id_DFFN, 7, 13, 1);
    // The reported frequency scales with the fraction of the period that is
    // available, which is 7/20 instead of 1/2.
    EXPECT_NEAR(duty.fmax, even.fmax * (7.0 / 20) / (12.5 / 25), 0.01 * even.fmax);
    // The slack used by the placer and router has to account for the shorter
    // window as well, not just for the path delay.
    EXPECT_NEAR(duty.slack_ns, even.slack_ns - 5.5, 0.01);
}

TEST(TimingSkewTest, OppositeEdgesUseTheClockLowTime)
{
    // Falling launch, rising capture: only the clock's low time is available.
    auto even = analyse_edge_pair(id_DFFN, id_DFF, 12.5, 12.5, 1);
    auto duty = analyse_edge_pair(id_DFFN, id_DFF, 7, 13, 1);
    EXPECT_NEAR(duty.fmax, even.fmax * (13.0 / 20) / (12.5 / 25), 0.01 * even.fmax);
    EXPECT_NEAR(duty.slack_ns, even.slack_ns + 0.5, 0.01);
}

// Synthetic sidecar only: these deliberately invented numbers test API/signs,
// not proprietary device timing or physical bounds.
static json11::Json modern_ff_fixture()
{
    using J = json11::Json;
    J::object records;
    for (std::string edge : {"Pos", "Neg"}) {
        for (std::string mode : {"DI_M", "DI_FS", "CE", "LSR_SYN", "LSR_ASYN"}) {
            bool fs = mode == "DI_FS";
            records[mode + "_" + edge] = J::object{{"setup_ps", J::array{fs ? 10 : 200, fs ? 20 : 300}},
                                                  {"hold_ps", J::array{fs ? 10 : -70, fs ? 25 : -50}}};
        }
        records["CLK_Q_" + edge] = J::object{{"clock_q_ps", J::array{edge == "Pos" ? 100 : 150,
                                                                 edge == "Pos" ? 400 : 450}}};
    }
    return J::object{{"schema", "gowin-modern-ff-v1"},
                     {"profile", "GW5A25-V1.9.11.03-default-C1/I0"},
                     {"target_arch", "GW5A-25A"}, {"target_part", "GW5A-LV25MG121NC1/I0"},
                     {"source_device", "GW5A-25A"},
                     {"source_sha256", "5e723783372c005a584e7e502646675d68a83256d28018ab82b4a88bad8f235a"},
                     {"normalization", "slow_group_1_x1.25_fast_group_2_x1_outward_ps"},
                     {"aggregation", "all_four_source_slots_not_RF_resolved"}, {"records", records}};
}

TEST(ModernFFTest, StrictSidecarPairsAndProfile)
{
    GowinModernFF model;
    model.parse(modern_ff_fixture());
    EXPECT_EQ(model.constraints.at("DI_M_Pos").hold.maxDelay(), -50);
    EXPECT_EQ(model.clock_q.at("Neg").maxDelay(), 450);
    for (auto bad : std::vector<json11::Json>{json11::Json(), json11::Json::array{1}, json11::Json::array{2, 1},
                     json11::Json::array{0, 1.5}, json11::Json::array{0, 100001},
                     json11::Json::array{0, "1"}})
        EXPECT_ANY_THROW(GowinModernFF::pair(bad));
    auto json = modern_ff_fixture().object_items();
    json["profile"] = "other";
    EXPECT_ANY_THROW(model.parse(json));
    json = modern_ff_fixture().object_items();
    json["records"] = json11::Json::object{};
    EXPECT_ANY_THROW(model.parse(json));
    json = modern_ff_fixture().object_items();
    json["source_sha256"] = "unknown";
    EXPECT_ANY_THROW(model.parse(json));
}

TEST(ModernFFTest, RoutedMuxAndUnroutedEnvelope)
{
    init_share_dirname();
    ArchArgs args;
    args.device = "GW5A-LV25MG121NC1/I0";
    auto ctx = std::make_unique<Context>(args);
    ctx->uarch->init(ctx.get());
    ctx->late_init();
    GowinModernFF model;
    model.parse(modern_ff_fixture());
    auto *cell = ctx->createCell(ctx->id("ff"), id_DFF);
    auto *net = ctx->createNet(ctx->id("data"));
    cell->addInput(id_D);
    cell->connectPort(id_D, net);
    auto info = model.clocking(ctx.get(), cell, id_D, 0);
    EXPECT_EQ(info.setup.maxDelay(), 300);
    EXPECT_EQ(info.hold.maxDelay(), 25); // not M alone while unrouted
    EXPECT_EQ(model.data_mode(ctx.get(), cell), "ENVELOPE");
    EXPECT_ANY_THROW(model.data_mode(ctx.get(), cell, true));
    ctx->uarch->prePlace(); // initialize architecture placement bookkeeping
    for (std::string lane : {"0", "2", "4"}) {
        auto bel = ctx->getBelByNameStr("X11Y1/DFF" + lane);
        ASSERT_NE(bel, BelId());
        ctx->bindBel(bel, cell, STRENGTH_LOCKED);
        auto wire = ctx->getBelPinWire(bel, id_D);
        bool tested_m = false, tested_fs = false;
        for (auto pip : ctx->getPipsUphill(wire)) {
            auto name = ctx->getPipName(pip);
            if (name.size() != 3 || name[1] != ctx->id("XD" + lane) ||
                    (name[2] != ctx->id("F" + lane) && name[2] != ctx->id("SEL" + lane)))
                continue;
            net->wires[wire].pip = pip;
            bool fs = name[2] == ctx->id("F" + lane);
            tested_fs |= fs;
            tested_m |= !fs;
            EXPECT_EQ(model.data_mode(ctx.get(), cell, true), fs ? "DI_FS" : "DI_M");
            info = model.clocking(ctx.get(), cell, id_D, 0);
            EXPECT_EQ(info.setup.maxDelay(), fs ? 20 : 300);
            EXPECT_EQ(info.hold.maxDelay(), fs ? 25 : -50); // signed, not negated
        }
        EXPECT_TRUE(tested_m);
        EXPECT_TRUE(tested_fs);
        net->wires.clear();
        ctx->unbindBel(bel);
    }
    for (auto type : {id_DFF, id_DFFN, id_DFFE, id_DFFNE, id_DFFRE, id_DFFCE, id_DFFSE, id_DFFPE}) {
        cell->type = type;
        auto q = model.clocking(ctx.get(), cell, id_Q, 0);
        bool negative = type.in(id_DFFN, id_DFFNE);
        EXPECT_EQ(q.edge, negative ? FALLING_EDGE : RISING_EDGE);
        EXPECT_EQ(q.clockToQ.minDelay(), negative ? 150 : 100);
        EXPECT_EQ(q.clockToQ.maxDelay(), negative ? 450 : 400);
        for (auto port : {id_CE, id_RESET, id_CLEAR, id_SET, id_PRESET}) {
            auto control = model.clocking(ctx.get(), cell, port, 0);
            EXPECT_EQ(control.setup.maxDelay(), 300);
            EXPECT_EQ(control.hold.maxDelay(), -50);
        }
    }
    cell->type = id_DFFS;
    EXPECT_ANY_THROW(model.check_cell(ctx.get(), cell));
    cell->type = id_DFF;
    auto *driver = ctx->createCell(ctx->id("ofx"), id_MUX2_LUT5);
    driver->addOutput(id_O);
    driver->connectPort(id_O, net);
    EXPECT_ANY_THROW(model.data_mode(ctx.get(), cell));
}

TEST(ModernFFTest, SourceSpecificMuxGuards)
{
    init_share_dirname();
    ArchArgs args;
    args.device = "GW5A-LV25MG121NC1/I0";
    auto ctx = std::make_unique<Context>(args);
    ctx->uarch->init(ctx.get());
    ctx->late_init();
    GowinModernFF model;
    model.parse(modern_ff_fixture());
    auto *ff = ctx->createCell(ctx->id("ff"), id_DFFRE);
    auto *alu = ctx->createCell(ctx->id("alu"), id_ALU);
    auto *ram = ctx->createCell(ctx->id("ram"), id_RAM16SDP4);
    auto *sum = ctx->createNet(ctx->id("sum"));
    auto *dout = ctx->createNet(ctx->id("dout"));
    ff->addInput(id_D);
    ff->connectPort(id_D, sum);
    alu->addOutput(id_SUM);
    alu->connectPort(id_SUM, sum);
    auto ram_do = ctx->id("DO[0]"); // frontend expands the four-bit DO bus
    ram->addOutput(ram_do);
    ram->connectPort(ram_do, dout);
    EXPECT_EQ(model.data_mode(ctx.get(), ff), "ENVELOPE");
    ctx->uarch->prePlace();
    auto ff_bel = ctx->getBelByNameStr("X11Y1/DFF0");
    auto alu_bel = ctx->getBelByNameStr("X11Y1/ALU0");
    ctx->bindBel(ff_bel, ff, STRENGTH_LOCKED);
    ctx->bindBel(alu_bel, alu, STRENGTH_LOCKED);
    auto wire = ctx->getBelPinWire(ff_bel, id_D);
    PipId f_pip, sel_pip;
    for (auto pip : ctx->getPipsUphill(wire)) {
        auto name = ctx->getPipName(pip);
        if (name.size() == 3 && name[1] == ctx->id("XD0")) {
            if (name[2] == ctx->id("F0"))
                f_pip = pip;
            if (name[2] == ctx->id("SEL0"))
                sel_pip = pip;
        }
    }
    ASSERT_NE(f_pip, PipId());
    ASSERT_NE(sel_pip, PipId());
    sum->wires[wire].pip = f_pip;
    EXPECT_EQ(model.data_mode(ctx.get(), ff, true), "DI_FS");
    sum->wires[wire].pip = sel_pip;
    EXPECT_EQ(model.data_mode(ctx.get(), ff, true), "DI_M");
    ctx->unbindBel(alu_bel);
    sum->wires[wire].pip = f_pip;
    EXPECT_ANY_THROW(model.data_mode(ctx.get(), ff, true));
    sum->wires.clear();
    ff->disconnectPort(id_D);
    ff->connectPort(id_D, dout);
    dout->wires[wire].pip = sel_pip;
    EXPECT_EQ(model.data_mode(ctx.get(), ff, true), "DI_M");
    dout->wires[wire].pip = f_pip;
    EXPECT_ANY_THROW(model.data_mode(ctx.get(), ff, true));
    dout->wires.clear();
    ff->disconnectPort(id_D);
    ff->connectPort(id_D, sum);
    alu->disconnectPort(id_SUM);
    alu->addOutput(id_COUT);
    alu->connectPort(id_COUT, sum);
    EXPECT_ANY_THROW(model.data_mode(ctx.get(), ff));
}

NEXTPNR_NAMESPACE_END
