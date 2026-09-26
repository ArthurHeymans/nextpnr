#include "gtest/gtest.h"
#include "command.h"
#include "design_utils.h"
#include "nextpnr.h"
#define HIMBAECHEL_CONSTIDS "uarch/gowin/constids.inc"
#include "himbaechel_constids.h"
#include "himbaechel_helpers.h"
#include "uarch/gowin/gowin.h"
#include "uarch/gowin/gowin_utils.h"
#include "uarch/gowin/pack.h"

NEXTPNR_NAMESPACE_BEGIN

class GowinPllTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *input;

    void SetUp() override
    {
        init_share_dirname();
        ArchArgs args;
        args.device = "GW5A-LV25MG121NC1/I0";
        ctx = std::make_unique<Context>(args);
        ctx->uarch->init(ctx.get());
        ctx->late_init();
        input = ctx->createNet(ctx->id("input"));
        input->clkconstr = std::make_unique<ClockConstraint>();
        input->clkconstr->period = DelayPair(ctx->getDelayFromNS(20));
        input->clkconstr->high = DelayPair(ctx->getDelayFromNS(7));
        input->clkconstr->low = DelayPair(ctx->getDelayFromNS(13));
    }

    CellInfo *pll(IdString type = id_rPLL)
    {
        auto *cell = ctx->createCell(ctx->id("pll"), type);
        cell->addInput(id_CLKIN);
        cell->connectPort(id_CLKIN, input);
        for (const char *port : {"CLKOUT", "CLKOUTP", "CLKOUTD", "CLKOUTD3", "CLKOUT0", "CLKOUT1"}) {
            IdString id = ctx->id(port);
            cell->addOutput(id);
            cell->connectPort(id, ctx->createNet(id));
        }
        return cell;
    }

    double period(CellInfo *cell, const char *port)
    {
        return ctx->getDelayNS(cell->getPort(ctx->id(port))->clkconstr->period.minDelay());
    }
};

TEST_F(GowinPllTest, StaticDividersAndExistingConstraints)
{
    auto *cell = pll();
    cell->params[ctx->id("IDIV_SEL")] = Property(1);
    cell->params[ctx->id("FBDIV_SEL")] = Property("5");
    cell->getPort(id_CLKOUTP)->clkconstr = std::make_unique<ClockConstraint>(*input->clkconstr);
    GowinPacker packer(ctx.get());
    EXPECT_TRUE(packer.constrain_pll_outputs(*cell));
    EXPECT_NEAR(period(cell, "CLKOUT"), 20.0 / 3, 0.002);
    EXPECT_NEAR(period(cell, "CLKOUTD"), 40.0 / 3, 0.002);
    EXPECT_NEAR(period(cell, "CLKOUTD3"), 20, 0.002);
    EXPECT_NEAR(period(cell, "CLKOUTP"), 20, 0.002);
    EXPECT_FALSE(packer.constrain_pll_outputs(*cell));
}

TEST_F(GowinPllTest, BypassPreservesInputWaveform)
{
    auto *cell = pll();
    cell->params[ctx->id("CLKOUT_BYPASS")] = Property("true");
    GowinPacker(ctx.get()).constrain_pll_outputs(*cell);
    auto &clock = *cell->getPort(id_CLKOUT)->clkconstr;
    EXPECT_NEAR(ctx->getDelayNS(clock.high.minDelay()), 7, 0.002);
    EXPECT_NEAR(ctx->getDelayNS(clock.low.minDelay()), 13, 0.002);
    EXPECT_EQ(cell->getPort(id_CLKOUTD)->clkconstr, nullptr);
}

TEST_F(GowinPllTest, UnsupportedRpllConfigurations)
{
    auto *cell = pll();
    for (auto param : {"DYN_IDIV_SEL", "DYN_FBDIV_SEL", "DYN_ODIV_SEL", "DYN_DA_EN"}) {
        cell->params[ctx->id(param)] = Property("true");
        EXPECT_FALSE(GowinPacker(ctx.get()).constrain_pll_outputs(*cell));
        cell->params.erase(ctx->id(param));
    }
    cell->params[ctx->id("DUTYDA_SEL")] = Property("0100");
    EXPECT_FALSE(GowinPacker(ctx.get()).constrain_pll_outputs(*cell));
}

TEST_F(GowinPllTest, UnconstrainedInput)
{
    auto *cell = pll();
    input->clkconstr.reset();
    EXPECT_FALSE(GowinPacker(ctx.get()).constrain_pll_outputs(*cell));
}

TEST_F(GowinPllTest, PllaOutputsAndSpreadSpectrum)
{
    auto *cell = pll(id_PLLA);
    cell->params[ctx->id("MDIV_SEL")] = Property(24);
    cell->params[ctx->id("ODIV0_SEL")] = Property(10);
    cell->params[ctx->id("SSC_EN")] = Property("TRUE");
    EXPECT_FALSE(GowinPacker(ctx.get()).constrain_pll_outputs(*cell));
    cell->params.erase(ctx->id("SSC_EN"));
    EXPECT_TRUE(GowinPacker(ctx.get()).constrain_pll_outputs(*cell));
    EXPECT_NEAR(period(cell, "CLKOUT0"), 25.0 / 3, 0.002);
    EXPECT_EQ(cell->getPort(ctx->id("CLKOUT1"))->clkconstr, nullptr);
}

NEXTPNR_NAMESPACE_END
