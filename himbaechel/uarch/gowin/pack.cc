#include <algorithm>
#include <cctype>
#include <map>

#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"

#define HIMBAECHEL_CONSTIDS "uarch/gowin/constids.inc"
#include "himbaechel_constids.h"
#include "himbaechel_helpers.h"

#include "gowin.h"
#include "gowin_utils.h"
#include "pack.h"

#include <cinttypes>

NEXTPNR_NAMESPACE_BEGIN

// ===================================
// Global set/reset
// ===================================
void GowinPacker::pack_gsr(void)
{
    log_info("Pack GSR...\n");

    bool user_gsr = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;

        if (ci.type == id_GSR) {
            user_gsr = true;
            break;
        }
    }
    if (!user_gsr) {
        // make default GSR
        auto gsr_cell = std::make_unique<CellInfo>(ctx, id_GSR, id_GSR);
        gsr_cell->addInput(id_GSRI);
        gsr_cell->connectPort(id_GSRI, ctx->nets.at(ctx->id("$PACKER_VCC")).get());
        ctx->cells[gsr_cell->name] = std::move(gsr_cell);
    }
    if (ctx->verbose) {
        if (user_gsr) {
            log_info("Have user GSR\n");
        } else {
            log_info("No user GSR. Make one.\n");
        }
    }
}

// ===================================
// Pin function configuration via wires
// ===================================
void GowinPacker::pack_pincfg(void)
{
    if (!gwu.has_PINCFG()) {
        return;
    }
    log_info("Pack PINCFG...\n");

    auto pincfg_cell = std::make_unique<CellInfo>(ctx, id_PINCFG, id_PINCFG);

    const int pin_cnt = gwu.has_I2CCFG() ? 5 : 4;
    for (int i = 0; i < pin_cnt; ++i) {
        IdString port = ctx->idf("UNK%d_VCC", i);
        pincfg_cell->addInput(port);
        if (i && gwu.need_CFGPINS_INVERSION()) {
            pincfg_cell->connectPort(port, ctx->nets.at(ctx->id("$PACKER_GND")).get());
        } else {
            pincfg_cell->connectPort(port, ctx->nets.at(ctx->id("$PACKER_VCC")).get());
        }
    }

    const ArchArgs &args = ctx->args;

    pincfg_cell->addInput(id_SSPI);
    if (args.options.count("sspi_as_gpio")) {
        pincfg_cell->connectPort(id_SSPI, ctx->nets.at(ctx->id("$PACKER_VCC")).get());
        pincfg_cell->setParam(id_SSPI, 1);
    } else {
        pincfg_cell->connectPort(id_SSPI, ctx->nets.at(ctx->id("$PACKER_GND")).get());
    }

    if (gwu.has_I2CCFG()) {
        pincfg_cell->addInput(id_I2C);
        if (args.options.count("i2c_as_gpio")) {
            pincfg_cell->connectPort(id_I2C, ctx->nets.at(ctx->id("$PACKER_VCC")).get());
            pincfg_cell->setParam(id_I2C, 1);
        } else {
            pincfg_cell->connectPort(id_I2C, ctx->nets.at(ctx->id("$PACKER_GND")).get());
        }
    }
    ctx->cells[pincfg_cell->name] = std::move(pincfg_cell);
}

// ===================================
// Global power regulator
// ===================================
void GowinPacker::pack_bandgap(void)
{
    if (!gwu.has_BANDGAP()) {
        return;
    }
    log_info("Pack BANDGAP...\n");

    bool user_bandgap = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;

        if (ci.type == id_BANDGAP) {
            user_bandgap = true;
            break;
        }
    }
    if (!user_bandgap) {
        // make default BANDGAP
        auto bandgap_cell = std::make_unique<CellInfo>(ctx, id_BANDGAP, id_BANDGAP);
        bandgap_cell->addInput(id_BGEN);
        bandgap_cell->connectPort(id_BGEN, ctx->nets.at(ctx->id("$PACKER_VCC")).get());
        ctx->cells[bandgap_cell->name] = std::move(bandgap_cell);
    }
    if (ctx->verbose) {
        if (user_bandgap) {
            log_info("Have user BANDGAP\n");
        } else {
            log_info("No user BANDGAP. Make one.\n");
        }
    }
}

// ===================================
// Replace INV with LUT
// ===================================
void GowinPacker::pack_inv(void)
{
    log_info("Pack INV...\n");

    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;

        if (ci.type == id_INV) {
            ci.type = id_LUT4;
            ci.renamePort(id_O, id_F);
            ci.renamePort(id_I, id_I3); // use D - it's simple for INIT
            ci.params[id_INIT] = Property(0x00ff);
        }
    }
}

// ===================================
// PLL
// ===================================
// Derive clock constraints for the PLL outputs from a constrained CLKIN, for
// static configurations with internal feedback. Outputs that already have a
// constraint keep it. Returns whether a constraint was added.
bool GowinPacker::constrain_pll_outputs(CellInfo &ci)
{
    NetInfo *clkin = ci.getPort(id_CLKIN);
    if (clkin == nullptr || clkin->clkconstr == nullptr) {
        return false;
    }
    double in_period = ctx->getDelayNS(clkin->clkconstr->period.minDelay());

    auto str_param = [&](const char *name, const char *def) {
        auto it = ci.params.find(ctx->id(name));
        std::string val = it == ci.params.end() ? def : it->second.as_string();
        std::transform(val.begin(), val.end(), val.begin(), [](unsigned char c) { return std::toupper(c); });
        return val;
    };
    // Dividers may be given as integers or as decimal strings.
    auto int_param = [&](const char *name, int64_t def) -> int64_t {
        auto it = ci.params.find(ctx->id(name));
        if (it == ci.params.end()) {
            return def;
        }
        if (it->second.is_string) {
            std::string val = it->second.as_string();
            size_t end = 0;
            try {
                int64_t num = std::stoll(val, &end);
                if (end == val.size()) {
                    return num;
                }
            } catch (const std::exception &) {
            }
            return -1;
        }
        return it->second.as_int64();
    };
    bool changed = false;
    auto constrain = [&](IdString port, double period, bool bypass = false) {
        NetInfo *net = ci.getPort(port);
        if (net == nullptr || net->clkconstr != nullptr || period <= 0) {
            return;
        }
        changed = true;
        net->clkconstr = std::make_unique<ClockConstraint>();
        if (bypass) {
            *net->clkconstr = *clkin->clkconstr;
        } else {
            net->clkconstr->period = DelayPair(ctx->getDelayFromNS(period));
            net->clkconstr->high = DelayPair(ctx->getDelayFromNS(period / 2));
            net->clkconstr->low = DelayPair(ctx->getDelayFromNS(period / 2));
        }
        log_info("    Derived frequency constraint of %.2f MHz for net %s\n", 1000.0 / period, ctx->nameOf(net));
    };

    if (ci.type.in(id_rPLL, id_PLLVR)) {
        if (str_param("CLKFB_SEL", "internal") != "INTERNAL" || str_param("DYN_IDIV_SEL", "false") != "FALSE" ||
            str_param("DYN_FBDIV_SEL", "false") != "FALSE" || str_param("DYN_ODIV_SEL", "false") != "FALSE" ||
            str_param("DYN_DA_EN", "false") != "FALSE" || str_param("DUTYDA_SEL", "1000") != "1000") {
            return false;
        }
        int64_t idiv = int_param("IDIV_SEL", 0), fbdiv = int_param("FBDIV_SEL", 0);
        int64_t sdiv = int_param("DYN_SDIV_SEL", 2);
        if (idiv < 0 || idiv > 63 || fbdiv < 0 || fbdiv > 63 || sdiv < 2 || sdiv > 128 || sdiv % 2 != 0) {
            return false;
        }
        // CLKOUT = FCLKIN * (FBDIV_SEL + 1) / (IDIV_SEL + 1); CLKOUTP is
        // CLKOUT phase shifted, CLKOUTD divides CLKOUT or CLKOUTP by
        // DYN_SDIV_SEL and CLKOUTD3 by three. The bypass options pass CLKIN
        // straight to an output; CLKOUTD and CLKOUTD3 are left alone when
        // their source is bypassed.
        double period = in_period * (idiv + 1) / (fbdiv + 1);
        bool bypass = str_param("CLKOUT_BYPASS", "false") == "TRUE";
        bool bypass_p = str_param("CLKOUTP_BYPASS", "false") == "TRUE";
        bool bypass_d = str_param("CLKOUTD_BYPASS", "false") == "TRUE";
        auto div_src_bypassed = [&](const char *src) {
            return str_param(src, "CLKOUT") == "CLKOUTP" ? bypass_p : bypass;
        };
        constrain(id_CLKOUT, bypass ? in_period : period, bypass);
        constrain(id_CLKOUTP, bypass_p ? in_period : period, bypass_p);
        if (bypass_d) {
            constrain(id_CLKOUTD, in_period, true);
        } else if (!div_src_bypassed("CLKOUTD_SRC")) {
            constrain(id_CLKOUTD, period * sdiv);
        }
        if (!div_src_bypassed("CLKOUTD3_SRC")) {
            constrain(id_CLKOUTD3, period * 3);
        }
        return changed;
    }

    if (ci.type == id_PLLA) {
        NetInfo *mdclk = ci.getPort(ctx->id("MDCLK"));
        if (str_param("CLKFB_SEL", "INTERNAL") != "INTERNAL" || int_param("MDIV_FRAC_SEL", 0) != 0 ||
            int_param("ODIV0_FRAC_SEL", 0) != 0 || str_param("SSC_EN", "FALSE") != "FALSE" ||
            str_param("DYN_DPA_EN", "FALSE") != "FALSE" ||
            (mdclk != nullptr && mdclk->name != ctx->id("$PACKER_GND"))) {
            return false;
        }
        int64_t idiv = int_param("IDIV_SEL", 1), fbdiv = int_param("FBDIV_SEL", 1);
        int64_t mdiv = int_param("MDIV_SEL", 8);
        if (idiv <= 0 || fbdiv <= 0 || mdiv <= 0) {
            return false;
        }
        // VCO = FCLKIN / IDIV_SEL * FBDIV_SEL * MDIV_SEL and CLKOUTi =
        // VCO / ODIVi_SEL, for enabled outputs whose divider is not cascaded.
        double vco_period = in_period * idiv / (double(fbdiv) * mdiv);
        for (int i = 0; i <= 6; ++i) {
            if (str_param(stringf("CLKOUT%d_EN", i).c_str(), i == 0 ? "TRUE" : "FALSE") != "TRUE" ||
                int_param(stringf("CLK%d_IN_SEL", i).c_str(), 0) != 0 ||
                int_param(stringf("CLK%d_OUT_SEL", i).c_str(), 0) != 0 ||
                str_param(stringf("DE%d_EN", i).c_str(), "FALSE") != "FALSE" ||
                str_param(stringf("DYN_PE%d_SEL", i).c_str(), "FALSE") != "FALSE") {
                continue;
            }
            int64_t odiv = int_param(stringf("ODIV%d_SEL", i).c_str(), 8);
            if (odiv > 0) {
                constrain(ctx->idf("CLKOUT%d", i), vco_period * odiv);
            }
        }
    }
    return changed;
}

void GowinPacker::pack_pll(void)
{
    log_info("Pack PLL...\n");

    pool<BelId> used_pll_bels;

    // Derive the output constraints before CLKIN may be disconnected for a
    // dedicated input below. Repeat until nothing changes so that a PLL fed
    // by another PLL is covered whatever the cell order.
    for (bool changed = true; changed;) {
        changed = false;
        for (auto &cell : ctx->cells) {
            auto &ci = *cell.second;
            if (ci.type.in(id_rPLL, id_PLLVR, id_PLLA)) {
                gwu.remove_brackets(&ci);
                changed |= constrain_pll_outputs(ci);
            }
        }
    }

    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;

        if (ci.type.in(id_rPLL, id_PLLVR, id_PLLA)) {

            // If CLKIN is connected to a special pin, then it makes sense
            // to try to place the PLL so that it uses a direct connection
            // to this pin.
            if (ci.bel == BelId()) {
                NetInfo *ni = ci.getPort(id_CLKIN);
                if (ni && ni->driver.cell && ni->driver.cell->bel != BelId()) {
                    BelId pll_bel = gwu.get_pll_bel(ni->driver.cell->bel, id_CLKIN_T);
                    if (ctx->debug) {
                        log_info("PLL clkin driver:%s at %s, PLL bel:%s\n", ctx->nameOf(ni->driver.cell),
                                 ctx->getBelName(ni->driver.cell->bel).str(ctx).c_str(),
                                 pll_bel != BelId() ? ctx->getBelName(pll_bel).str(ctx).c_str() : "NULL");
                    }
                    if (pll_bel != BelId() && used_pll_bels.count(pll_bel) == 0) {
                        used_pll_bels.insert(pll_bel);
                        ctx->bindBel(pll_bel, &ci, PlaceStrength::STRENGTH_LOCKED);
                        ci.disconnectPort(id_CLKIN);
                        ci.setParam(id_INSEL, std::string("CLKIN0"));
                    }
                }
            }
        }
    }
}

// ===================================
// ADC
// ===================================
void GowinPacker::pack_adc(void)
{
    log_info("Pack ADC...\n");

    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;

        if (is_adc(&ci)) {
            gwu.remove_brackets(&ci);
        }
    }
}

// ===================================
// GW JTAG
// ===================================
void GowinPacker::pack_gw_jtag(void)
{
    log_info("Pack GW JTAG...\n");

    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;

        if (is_gw_jtag(&ci)) {
            // disconnect pads - they have no wires
            ci.disconnectPort(ctx->id("tck_pad_i"));
            ci.disconnectPort(ctx->id("tms_pad_i"));
            ci.disconnectPort(ctx->id("tdi_pad_i"));
            ci.disconnectPort(ctx->id("tdo_pad_o"));
            ci.disconnectPort(ctx->id("pause_dr_o"));
        }
    }
}

// ===================================
// HCLK -- CLKDIV and CLKDIV2 for now
// ===================================
void GowinPacker::pack_hclk(void)
{
    log_info("Pack HCLK cells...\n");

    // In the GW5A series, the CLKDIV2 can simultaneously transmit a signal to
    // both IOLOGIC and CLKDIV.
    if (gwu.has_5A_HCLK()) {
        return;
    }
    for (auto &cell : ctx->cells) {
        auto ci = cell.second.get();
        if (ci->type != id_CLKDIV)
            continue;
        NetInfo *hclk_in = ci->getPort(id_HCLKIN);
        if (hclk_in) {
            CellInfo *this_driver = hclk_in->driver.cell;
            if (this_driver && this_driver->type == id_CLKDIV2) {
                NetInfo *out = this_driver->getPort(id_CLKOUT);
                if (out->users.entries() > 1) {
                    // We could do as the IDE does sometimes and replicate the CLKDIV2 cell
                    // as many times as we need. For now, we keep things simple
                    log_error("CLKDIV2 that drives CLKDIV should drive no other cells\n");
                }
                ci->cluster = ci->name;
                this_driver->cluster = ci->name;
                ci->constr_children.push_back(this_driver);
                this_driver->constr_x = 0;
                this_driver->constr_y = 0;
                this_driver->constr_z = BelZ::CLKDIV2_0_Z - BelZ::CLKDIV_0_Z;
                this_driver->constr_abs_z = false;
            }
        }
    }
}

// ===================================
// DLLDLY
// ===================================
void GowinPacker::pack_dlldly(void)
{
    log_info("Pack DLLDLYs...\n");

    for (auto &cell : ctx->cells) {
        auto ci = cell.second.get();
        if (ci->type != id_DLLDLY)
            continue;
        NetInfo *clkin_net = ci->getPort(id_CLKIN);
        NetInfo *clkout_net = ci->getPort(id_CLKOUT);
        if (clkin_net == nullptr || clkout_net == nullptr) {
            log_error("%s cell has unconnected CLKIN or CLKOUT pins.\n", ctx->nameOf(ci));
        }
        CellInfo *clk_src = clkin_net->driver.cell;
        if (!is_io(clk_src)) {
            log_error("Clock source for DLLDLY %s is not IO: %s.\n", ctx->nameOf(ci), ctx->nameOf(clk_src));
        }
        // DLLDLY placement is fixed
        BelId io_bel = clk_src->bel;
        BelId dlldly_bel = gwu.get_dlldly_bel(io_bel);
        if (dlldly_bel == BelId()) {
            log_error("Can't use IO %s as input for DLLDLY %s.\n", ctx->nameOf(clk_src), ctx->nameOf(ci));
        }
        if (ctx->verbose) {
            log_info("  pack %s to use clock pin at %s\n", ctx->nameOf(ci), ctx->nameOfBel(io_bel));
        }
        ctx->bindBel(dlldly_bel, ci, STRENGTH_LOCKED);
        gwu.remove_brackets(ci);
    }
}

// =========================================
// Create entry points to the clock system
// =========================================
void GowinPacker::pack_buffered_nets(void)
{
    log_info("Pack buffered nets...\n");

    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        if (ni->driver.cell == nullptr || ni->users.empty() || net.first == ctx->id("$PACKER_GND") ||
            net.first == ctx->id("$PACKER_VCC")) {
            continue;
        }
        if (ni->attrs.count(id_CLOCK) == 0) {
            if (ctx->settings.count(id_NO_GP_CLOCK_ROUTING)) {
                continue;
            }
            if (gwu.driver_is_clksrc(ni->driver) || (!gwu.driver_is_io(ni->driver))) {
                // no need for buffering
                continue;
            }
            // check users for the clock inputs
            bool has_clock_users = false;
            for (auto usr : ni->users) {
                if (usr.port.in(id_CLKIN, id_CLK, id_CLK0, id_CLK1, id_CLK2, id_CLK3, id_CLKFB)) {
                    // Latch gate signals drive CLK pins but are not clocks
                    if (usr.port == id_CLK && usr.cell->attrs.count(id_LATCH))
                        continue;
                    has_clock_users = true;
                    break;
                }
            }
            if (!has_clock_users) {
                continue;
            }
            if (ctx->verbose) {
                log_info("Add buffering to a potentially clock network '%s'\n", ctx->nameOf(ni));
            }
        }

        // make new BUF cell single user for the net driver
        IdString buf_name = ctx->idf("%s_BUFG", net.first.c_str(ctx));
        ctx->createCell(buf_name, id_BUFG);
        CellInfo *buf_ci = ctx->cells.at(buf_name).get();
        buf_ci->addInput(id_I);
        // move driver
        CellInfo *driver_cell = ni->driver.cell;
        IdString driver_port = ni->driver.port;

        driver_cell->movePortTo(driver_port, buf_ci, id_O);
        buf_ci->connectPorts(id_I, driver_cell, driver_port);
    }
}

// =========================================
// Create DQCEs
// =========================================
void GowinPacker::pack_dqce(void)
{
    log_info("Pack DQCE cells...\n");

    // At the placement stage, nothing can be said definitively about DQCE,
    // so we make user cells virtual but allocate all available bels by
    // creating and placing cells - we will use some of them after, and
    // delete the rest.
    // We do this here because the decision about which physical DQCEs to
    // use is made during routing, but some of the information (let’s say
    // mapping cell pins -> bel pins) is filled in before routing.
    bool grab_bels = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (ci.type == id_DQCE) {
            ci.pseudo_cell = std::make_unique<RegionPlug>(Loc(0, 0, 0));
            grab_bels = true;
        }
    }
    if (grab_bels) {
        for (int i = 0; i < 32; ++i) {
            BelId dqce_bel = gwu.get_dqce_bel(ctx->idf("SPINE%d", i));
            if (dqce_bel != BelId()) {
                IdString dqce_name = ctx->idf("$PACKER_DQCE_SPINE%d", i);
                CellInfo *dqce = ctx->createCell(dqce_name, id_DQCE);
                dqce->addInput(id_CE);
                ctx->bindBel(dqce_bel, dqce, STRENGTH_LOCKED);
            }
        }
    }
}

// =========================================
// Create DCSs
// =========================================
void GowinPacker::pack_dcs(void)
{
    log_info("Pack DCS cells...\n");

    // At the placement stage, nothing can be said definitively about DCS,
    // so we make user cells virtual but allocate all available bels by
    // creating and placing cells - we will use some of them after, and
    // delete the rest.
    // We do this here because the decision about which physical DCEs to
    // use is made during routing, but some of the information (let’s say
    // mapping cell pins -> bel pins) is filled in before routing.
    bool grab_bels = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (ci.type == id_DCS) {
            ci.pseudo_cell = std::make_unique<RegionPlug>(Loc(0, 0, 0));
            grab_bels = true;
        }
    }
    if (grab_bels) {
        for (int i = 0; i < 8; ++i) {
            BelId dcs_bel = gwu.get_dcs_bel(ctx->idf("P%d%dA", 1 + (i % 4), 6 + (i >> 2)));
            if (dcs_bel != BelId()) {
                IdString dcs_name = ctx->idf("$PACKER_DCS_SPINE%d", 8 * (i % 4) + 6 + (i >> 2));
                CellInfo *dcs = ctx->createCell(dcs_name, id_DCS);
                ctx->copyBelPorts(dcs_name, dcs_bel);
                ctx->bindBel(dcs_bel, dcs, STRENGTH_LOCKED);
            }
        }
    }
}

// =========================================
// Create DHCENs
// =========================================
void GowinPacker::pack_dhcens(void)
{
    // Allocate all available dhcen bels; we will find out which of them
    // will actually be used during the routing process.
    bool grab_bels = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (ci.type == id_DHCEN) {
            ci.pseudo_cell = std::make_unique<RegionPlug>(Loc(0, 0, 0));
            grab_bels = true;
        }
    }
    if (grab_bels) {
        // sane message if new primitives are used with old bases
        auto buckets = ctx->getBelBuckets();
        NPNR_ASSERT_MSG(std::find(buckets.begin(), buckets.end(), id_DHCEN) != buckets.end(),
                        "There are no DHCEN bels to use.");
        int i = 0;
        for (auto &bel : ctx->getBelsInBucket(ctx->getBelBucketForCellType(id_DHCEN))) {
            IdString dhcen_name = ctx->idf("$PACKER_DHCEN_%d", ++i);
            CellInfo *dhcen = ctx->createCell(dhcen_name, id_DHCEN);
            dhcen->addInput(id_CE);
            ctx->bindBel(bel, dhcen, STRENGTH_LOCKED);
        }
    }
}

// =========================================
// Enable UserFlash
// =========================================
void GowinPacker::pack_userflash(bool have_emcu)
{
    log_info("Pack UserFlash cells...\n");
    std::vector<std::unique_ptr<CellInfo>> new_cells;

    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (!is_userflash(&ci)) {
            continue;
        }

        if (ci.type.in(id_FLASH96K, id_FLASH256K, id_FLASH608K)) {
            // enable
            ci.addInput(id_INUSEN);
            ci.connectPort(id_INUSEN, ctx->nets.at(ctx->id("$PACKER_GND")).get());
        }
        gwu.remove_brackets(&ci);

        if (have_emcu) {
            continue;
        }

        // add invertor
        int lut_idx = 0;
        auto add_inv = [&](IdString port, PortType port_type) {
            if (!gwu.port_used(&ci, port)) {
                return;
            }

            std::unique_ptr<CellInfo> lut_cell =
                    gwu.create_cell(gwu.create_aux_name(ci.name, lut_idx, "_lut$"), id_LUT4);
            new_cells.push_back(std::move(lut_cell));
            CellInfo *lut = new_cells.back().get();
            lut->addInput(id_I0);
            lut->addOutput(id_F);
            lut->setParam(id_INIT, 0x5555);
            ++lut_idx;

            if (port_type == PORT_IN) {
                ci.movePortTo(port, lut, id_I0);
                lut->connectPorts(id_F, &ci, port);
            } else {
                ci.movePortTo(port, lut, id_F);
                ci.connectPorts(port, lut, id_I0);
            }
        };
        for (auto pin : ci.ports) {
            if (pin.second.type == PORT_OUT) {
                add_inv(pin.first, PORT_OUT);
            } else {
                if (pin.first == id_INUSEN) {
                    continue;
                }
                if (ci.type == id_FLASH608K && pin.first.in(id_XADR0, id_XADR1, id_XADR2, id_XADR3, id_XADR4, id_XADR5,
                                                            id_XADR6, id_XADR7, id_XADR8)) {
                    continue;
                }
                add_inv(pin.first, PORT_IN);
            }
        }
    }
    for (auto &ncell : new_cells) {
        ctx->cells[ncell->name] = std::move(ncell);
    }
}

// =========================================
// Create EMCU
// =========================================
void GowinPacker::pack_emcu_and_flash(void)
{
    log_info("Pack EMCU and UserFlash cells...\n");
    std::vector<std::unique_ptr<CellInfo>> new_cells;

    bool have_emcu = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (!is_emcu(&ci)) {
            continue;
        }
        have_emcu = true;

        gwu.remove_brackets(&ci);

        // The flash data bus is connected directly to the CPU so just disconnect these networks
        // also other non-switched networks
        ci.disconnectPort(ctx->id("DAPNTDOEN"));
        ci.disconnectPort(ctx->id("DAPNTRST"));
        ci.disconnectPort(ctx->id("DAPTDO"));
        ci.disconnectPort(ctx->id("DAPTDI"));
        ci.disconnectPort(ctx->id("TARGFLASH0HREADYMUX"));
        ci.disconnectPort(ctx->id("TARGEXP0HAUSER"));
        ci.disconnectPort(ctx->id("TARGFLASH0EXRESP"));
        ci.disconnectPort(ctx->id("PORESETN"));
        ci.disconnectPort(ctx->id("SYSRESETN"));
        ci.disconnectPort(ctx->id("DAPSWDITMS"));
        ci.disconnectPort(ctx->id("DAPSWCLKTCK"));
        ci.disconnectPort(ctx->id("TPIUTRACECLK"));
        for (int i = 0; i < 32; ++i) {
            if (i < 4) {
                if (i < 3) {
                    ci.disconnectPort(ctx->idf("TARGFLASH0HSIZE%d", i));
                    ci.disconnectPort(ctx->idf("TARGFLASH0HBURST%d", i));
                    ci.disconnectPort(ctx->idf("TARGFLASH0HRUSER%d", i));
                    ci.disconnectPort(ctx->idf("INITEXP0HRUSER%d", i));
                }
                // ci.disconnectPort(ctx->idf("TARGFLASH0HPROT%d", i));
                ci.disconnectPort(ctx->idf("TARGEXP0HWUSER%d", i));
                ci.disconnectPort(ctx->idf("MTXREMAP%d", i));
            }
            // ins
            ci.disconnectPort(ctx->idf("TARGFLASH0HRDATA%d", i));
        }
    }
    pack_userflash(have_emcu);
}

void GowinPacker::run(void)
{
    handle_constants();
    pack_iobs();
    ctx->check();

    pack_i3c();
    ctx->check();

    pack_mipi();
    ctx->check();

    pack_diff_iobs();
    ctx->check();

    pack_io_regs();
    ctx->check();

    pack_iodelay();
    ctx->check();

    pack_iem();
    ctx->check();

    pack_iologic();
    ctx->check();

    pack_io16();
    ctx->check();

    pack_gsr();
    ctx->check();

    pack_pincfg();
    ctx->check();

    pack_hclk();
    ctx->check();

    pack_dlldly();
    ctx->check();

    pack_bandgap();
    ctx->check();

    pack_wideluts();
    ctx->check();

    pack_alus();
    ctx->check();

    pack_ssram();
    ctx->check();

    pack_latches();
    ctx->check();

    constrain_lutffs();
    ctx->check();

    pack_pll();
    ctx->check();

    pack_adc();
    ctx->check();

    pack_gw_jtag();
    ctx->check();

    pack_bsram();
    ctx->check();

    pack_dsp();
    ctx->check();

    pack_inv();
    ctx->check();

    pack_buffered_nets();
    ctx->check();

    pack_emcu_and_flash();
    ctx->check();

    pack_dhcens();
    ctx->check();

    pack_dqce();
    ctx->check();

    pack_dcs();
    ctx->check();

    ctx->fixupHierarchy();
    ctx->check();
}

void gowin_pack(Context *ctx)
{
    GowinPacker packer(ctx);
    packer.run();
}

NEXTPNR_NAMESPACE_END
