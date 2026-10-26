#include <sstream>

#include "gtest/gtest.h"
#include "command.h"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

TEST(GowinSdcTest, MissingPortAndCellSelectorsWarn)
{
    init_share_dirname();
    ArchArgs args;
    args.device = "GW5A-LV25MG121NC1/I0";
    auto ctx = std::make_unique<Context>(args);
    ctx->uarch->init(ctx.get());
    ctx->late_init();

    bool old_warn_as_error = log_warn_as_error;
    bool old_nonfatal_error = had_nonfatal_error;
    log_warn_as_error = true;

    had_nonfatal_error = false;
    std::istringstream port_sdc("create_clock -period 25 [get_ports {missing_port}]\n");
    ctx->read_sdc(port_sdc);
    EXPECT_TRUE(had_nonfatal_error);

    had_nonfatal_error = false;
    std::istringstream cell_sdc("get_cells {missing_cell}\n");
    ctx->read_sdc(cell_sdc);
    EXPECT_TRUE(had_nonfatal_error);

    IdString clock = ctx->id("clock");
    NetInfo *net = ctx->createNet(clock);
    ctx->ports[clock] = PortInfo{clock, net, PORT_IN};
    had_nonfatal_error = false;
    std::istringstream valid_sdc("create_clock -period 25 [get_ports {clock}]\n");
    ctx->read_sdc(valid_sdc);
    EXPECT_FALSE(had_nonfatal_error);
    EXPECT_NE(net->clkconstr, nullptr);

    log_warn_as_error = old_warn_as_error;
    had_nonfatal_error = old_nonfatal_error;
}

NEXTPNR_NAMESPACE_END
