#include <sstream>

#include "command.h"
#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"
#define HIMBAECHEL_CONSTIDS "uarch/gowin/constids.inc"
#include "himbaechel_constids.h"
#include "uarch/gowin/cst.h"

NEXTPNR_NAMESPACE_BEGIN

class GowinCstTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    bool old_warn_as_error = log_warn_as_error;
    bool old_nonfatal_error = had_nonfatal_error;

    void SetUp() override
    {
        init_share_dirname();
        ArchArgs args;
        args.device = "GW5A-LV25MG121NC1/I0";
        ctx = std::make_unique<Context>(args);
        ctx->uarch->init(ctx.get());
        ctx->late_init();
        log_warn_as_error = true;
        had_nonfatal_error = false;
    }

    void TearDown() override
    {
        log_warn_as_error = old_warn_as_error;
        had_nonfatal_error = old_nonfatal_error;
    }

    std::string first_package_pin()
    {
        const PadInfoPOD *pads = ctx->package_info->pads.get();
        for (size_t i = 0; i < ctx->package_info->pads.ssize(); i++) {
            std::string pin = IdString(pads[i].package_pin).c_str(ctx.get());
            if (!pin.empty())
                return pin;
        }
        return "";
    }
};

TEST_F(GowinCstTest, UnmatchedCellWarns)
{
    std::istringstream cst("IO_LOC \"missing\" A2;\n");
    EXPECT_TRUE(gowin_apply_constraints(ctx.get(), cst));
    EXPECT_TRUE(had_nonfatal_error);
}

TEST_F(GowinCstTest, UnmatchedClockNetWarns)
{
    std::istringstream cst("CLOCK_LOC \"missing\" BUFG;\n");
    EXPECT_TRUE(gowin_apply_constraints(ctx.get(), cst));
    EXPECT_TRUE(had_nonfatal_error);
}

TEST_F(GowinCstTest, MatchedConstraintDoesNotWarn)
{
    std::string pin = first_package_pin();
    ASSERT_FALSE(pin.empty());
    CellInfo *led = ctx->createCell(ctx->id("led"), ctx->id("IOB"));

    std::istringstream cst("IO_LOC \"led\" " + pin + ";\n");
    EXPECT_TRUE(gowin_apply_constraints(ctx.get(), cst));
    EXPECT_FALSE(had_nonfatal_error);
    EXPECT_TRUE(led->attrs.count(ctx->id("BEL")));
}

NEXTPNR_NAMESPACE_END
