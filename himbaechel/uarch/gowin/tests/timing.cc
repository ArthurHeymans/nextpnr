#include "timing.h"
#include "command.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
#define HIMBAECHEL_CONSTIDS "uarch/gowin/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

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

NEXTPNR_NAMESPACE_END
