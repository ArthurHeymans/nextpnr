#ifndef GOWIN_MODERN_FF_H
#define GOWIN_MODERN_FF_H

#include <cmath>
#include <fstream>
#include <map>

#include "nextpnr.h"
#ifndef HIMBAECHEL_CONSTIDS
#define HIMBAECHEL_CONSTIDS "uarch/gowin/constids.inc"
#endif
#include "himbaechel_constids.h"
#include "gowin.h"
#include "json11.hpp"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

// Explicit sidecar, deliberately independent of cached chipdb regeneration.
// This is an experimental source-slot envelope, NOT complete device STA.
struct GowinModernFF
{
    struct Constraint
    {
        DelayPair setup, hold;
    };
    std::map<std::string, Constraint> constraints;
    std::map<std::string, DelayPair> clock_q;
    std::string source_sha256;

    static void require(bool valid, const std::string &message)
    {
        if (!valid)
            log_error("modern_ff_timing: %s\n", message.c_str());
    }

    static DelayPair pair(const json11::Json &value)
    {
        require(value.is_array() && value.array_items().size() == 2, "missing timing pair");
        for (const auto &v : value.array_items())
            require(v.is_number() && std::isfinite(v.number_value()) && std::abs(v.number_value()) <= 100000 &&
                            v.number_value() == std::floor(v.number_value()),
                    "timings must be finite integer ps within range");
        require(value[0].int_value() <= value[1].int_value(), "reversed timing pair");
        return DelayPair(value[0].int_value(), value[1].int_value());
    }

    void parse(const json11::Json &json)
    {
        require(json["schema"].string_value() == "gowin-modern-ff-v1" &&
                        json["profile"].string_value() == "GW5A25-V1.9.11.03-default-C1/I0" &&
                        json["target_arch"].string_value() == "GW5A-25A" &&
                        json["target_part"].string_value() == "GW5A-LV25MG121NC1/I0" &&
                        json["normalization"].string_value() == "slow_group_1_x1.25_fast_group_2_x1_outward_ps" &&
                        json["aggregation"].string_value() == "all_four_source_slots_not_RF_resolved",
                "unsupported schema/device/profile/normalization");
        source_sha256 = json["source_sha256"].string_value();
        require((source_sha256 == "5e723783372c005a584e7e502646675d68a83256d28018ab82b4a88bad8f235a" &&
                 json["source_device"].string_value() == "GW5A-25A") ||
                        (source_sha256 == "26d54468bff1e2301fbd9bd121d92392bdd3f4fddaddbe6d33edbfd2dcddb166" &&
                         json["source_device"].string_value() == "GW5A-25B"),
                "unwitnessed source identity");
        const auto &records = json["records"];
        require(records.is_object() && records.object_items().size() == 12, "incomplete/unknown record set");
        constraints.clear();
        clock_q.clear();
        for (std::string edge : {"Pos", "Neg"}) {
            for (std::string mode : {"DI_M", "DI_FS", "CE", "LSR_SYN", "LSR_ASYN"}) {
                auto &record = records[mode + "_" + edge];
                require(record.is_object() && record.object_items().size() == 2, "invalid constraint record");
                constraints.emplace(mode + "_" + edge, Constraint{pair(record["setup_ps"]), pair(record["hold_ps"])});
            }
            auto &record = records["CLK_Q_" + edge];
            require(record.is_object() && record.object_items().size() == 1, "invalid clock-Q record");
            auto delay = pair(record["clock_q_ps"]);
            require(delay.minDelay() >= 0, "negative clock-Q delay");
            clock_q.emplace(edge, delay);
        }
    }

    void load(const std::string &filename)
    {
        std::ifstream input(filename, std::ios::binary | std::ios::ate);
        require(input.good() && input.tellg() >= 0 && input.tellg() <= 128 * 1024, "unreadable/oversized sidecar");
        std::string text(size_t(input.tellg()), '\0');
        input.seekg(0);
        input.read(text.data(), text.size());
        require(bool(input), "short sidecar read");
        std::string error;
        auto json = json11::Json::parse(text, error);
        require(error.empty(), "invalid JSON: " + error);
        parse(json);
    }

    void check_cell(Context *ctx, const CellInfo *cell) const
    {
        // Only fixture-covered primitive types; do not silently approximate
        // latches, other reset polarities or packed I/O registers.
        require(cell->type.in(id_DFF, id_DFFN, id_DFFE, id_DFFNE, id_DFFRE, id_DFFCE, id_DFFSE, id_DFFPE),
                "unsupported FF type " + cell->type.str(ctx));
    }

    std::string data_mode(Context *ctx, const CellInfo *cell, bool final = false) const
    {
        auto net = cell->getPort(id_D);
        require(net != nullptr, "FF has no data net");
        const auto *driver = net->driver.cell;
        bool alu = driver && is_alu(driver);
        bool ram = driver && driver->type == id_RAM16SDP4;
        bool ram_do = ram && net->driver.port.in(ctx->id("DO[0]"), ctx->id("DO[1]"),
                                                 ctx->id("DO[2]"), ctx->id("DO[3]"));
        // ALU SUM->F/SEL and RAM16 DO->SEL have dedicated paired fixtures.
        // OFX and other uncharacterized source/mux combinations stay rejected.
        if (driver)
            require(is_lut(driver) || is_dff(driver) || is_io(driver) ||
                            driver->type.in(id_GOWIN_VCC, id_GOWIN_GND) ||
                            (alu && net->driver.port == id_SUM) || ram_do,
                    "unsupported FF data driver for " + cell->name.str(ctx) + ": " + driver->type.str(ctx) + "." +
                            net->driver.port.str(ctx));
        if (cell->bel != BelId()) {
            auto wire = ctx->getBelPinWire(cell->bel, id_D);
            auto found = net->wires.find(wire);
            if (found != net->wires.end() && found->second.pip != PipId()) {
                // Inspect the pip's original tile-local endpoint names, not
                // normalized wire types: shared/aliased SEL wires (notably
                // control lanes) need not retain the MUX_SEL wire type.
                auto name = ctx->getPipName(found->second.pip);
                auto lane = std::to_string(ctx->getBelLocation(cell->bel).z / 2);
                require(name.size() == 3 && name[1] == ctx->id("XD" + lane),
                        "unsupported FF data pip destination");
                if (name[2] == ctx->id("SEL" + lane)) {
                    return "DI_M";
                }
                if (name[2] == ctx->id("F" + lane)) {
                    require(!ram, "RAM16 through local F is not characterized");
                    if (alu) {
                        require(driver->bel != BelId(), "ALU data source is unplaced");
                        auto alu_name = ctx->getBelName(driver->bel), ff_name = ctx->getBelName(cell->bel);
                        require(alu_name[0] == ff_name[0] && alu_name[1] == ctx->id("ALU" + lane),
                                "ALU SUM is not paired with this FF");
                    }
                    return "DI_FS";
                }
                require(false, "unsupported routed FF data mux input");
            }
        }
        require(!final, "missing routed FF data input");
        return "ENVELOPE";
    }

    TimingPortClass port_class(Context *ctx, const CellInfo *cell, IdString port, int &count) const
    {
        check_cell(ctx, cell);
        count = 0;
        if (port == id_CLK)
            return TMG_CLOCK_INPUT;
        count = 1;
        if (port == id_Q)
            return TMG_REGISTER_OUTPUT;
        require(port.in(id_D, id_CE, id_RESET, id_CLEAR, id_SET, id_PRESET), "unsupported FF timing pin");
        return TMG_REGISTER_INPUT;
    }

    TimingClockingInfo clocking(Context *ctx, const CellInfo *cell, IdString port, int index) const
    {
        check_cell(ctx, cell);
        require(index == 0, "invalid clocking index");
        const bool negative = cell->type.in(id_DFFN, id_DFFNE);
        const std::string edge = negative ? "Neg" : "Pos";
        TimingClockingInfo result;
        result.clock_port = id_CLK;
        result.edge = negative ? FALLING_EDGE : RISING_EDGE;
        if (port == id_Q) {
            const auto &q = clock_q.at(edge);
            result.clockToQ = DelayQuad(q.minDelay(), q.maxDelay());
        } else {
            std::string mode;
            if (port == id_D)
                mode = data_mode(ctx, cell);
            else if (port == id_CE)
                mode = "CE";
            else if (port.in(id_RESET, id_SET))
                mode = "LSR_SYN";
            else if (port.in(id_CLEAR, id_PRESET))
                mode = "LSR_ASYN"; // recovery/removal represented by setup/hold API
            else
                require(false, "unsupported FF clocking pin");
            if (mode == "ENVELOPE") {
                const auto &m = constraints.at("DI_M_" + edge), &fs = constraints.at("DI_FS_" + edge);
                result.setup = DelayPair(std::min(m.setup.minDelay(), fs.setup.minDelay()),
                                         std::max(m.setup.maxDelay(), fs.setup.maxDelay()));
                result.hold = DelayPair(std::min(m.hold.minDelay(), fs.hold.minDelay()),
                                        std::max(m.hold.maxDelay(), fs.hold.maxDelay()));
            } else {
                const auto &c = constraints.at(mode + "_" + edge);
                result.setup = c.setup;
                result.hold = c.hold;
            }
        }
        return result;
    }
};

NEXTPNR_NAMESPACE_END

#endif
