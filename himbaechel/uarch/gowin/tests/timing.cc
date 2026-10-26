#include "gtest/gtest.h"
#include "command.h"
#include "nextpnr.h"
#include "timing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gowin/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

TEST(TimingSkewTest, OppositeEdgesShareTheClockTree)
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
    clk->clkconstr->period = DelayPair(ctx->getDelayFromNS(25));
    clk->clkconstr->high = DelayPair(ctx->getDelayFromNS(12.5));
    clk->clkconstr->low = DelayPair(ctx->getDelayFromNS(12.5));
    auto *data = ctx->createNet(ctx->id("data"));

    auto *src = ctx->createCell(ctx->id("src"), id_DFF);
    src->addInput(id_CLK);
    src->addOutput(id_Q);
    src->connectPort(id_CLK, clk);
    src->connectPort(id_Q, data);
    auto *dst = ctx->createCell(ctx->id("dst"), id_DFFN);
    dst->addInput(id_CLK);
    dst->addInput(id_D);
    dst->connectPort(id_CLK, clk);
    dst->connectPort(id_D, data);
    ctx->assignArchInfo();

    TimingAnalyser timing(ctx.get());
    timing.with_clock_skew = true;
    timing.setup(false, false, true);
    timing.set_route_delay(CellPortKey(dst->name, id_D), DelayPair(ctx->getDelayFromNS(1)));
    timing.set_route_delay(CellPortKey(src->name, id_CLK), DelayPair(0));
    timing.set_route_delay(CellPortKey(dst->name, id_CLK), DelayPair(0));
    timing.run(false, false, false, true);
    auto baseline = timing.get_timing_result().clock_fmax.at(clk->name).achieved;

    timing.set_route_delay(CellPortKey(src->name, id_CLK), DelayPair(ctx->getDelayFromNS(2)));
    timing.run(false, false, false, true);
    auto skewed = timing.get_timing_result().clock_fmax.at(clk->name).achieved;
    EXPECT_NEAR(skewed, 500.0 / (500.0 / baseline + 2.0), 0.01);

    timing.set_route_delay(CellPortKey(dst->name, id_CLK), DelayPair(ctx->getDelayFromNS(1)));
    timing.run(false, false, false, true);
    auto partially_cancelled = timing.get_timing_result().clock_fmax.at(clk->name).achieved;
    EXPECT_NEAR(partially_cancelled, 500.0 / (500.0 / baseline + 1.0), 0.01);
}

NEXTPNR_NAMESPACE_END
