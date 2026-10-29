/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Claire Xenia Wolf <claire@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <algorithm>
#include <filesystem>
#include <fstream>
#include "command.h"
#include "design_utils.h"
#include "log.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

class HimbaechelCommandHandler : public CommandHandler
{
  public:
    HimbaechelCommandHandler(int argc, char **argv);
    virtual ~HimbaechelCommandHandler() {};
    std::unique_ptr<Context> createContext(dict<std::string, Property> &values) override;
    void setupArchContext(Context *ctx) override;
    void customAfterLoad(Context *ctx) override;
    void customBitstream(Context *ctx) override;

  protected:
    void validate() override;
    po::options_description getArchOptions() override;
};

HimbaechelCommandHandler::HimbaechelCommandHandler(int argc, char **argv) : CommandHandler(argc, argv)
{
    init_share_dirname();
}

void HimbaechelCommandHandler::validate()
{
    if (vm.count("timing-only-unconstrained-clock") && !vm.count("timing-only"))
        log_error("--timing-only-unconstrained-clock requires --timing-only.\n");
    if (!vm.count("timing-only"))
        return;
    for (const char *required : {"json", "sdc", "report", "write", "no-pack", "no-place", "no-route"})
        if (!vm.count(required))
            log_error("--timing-only requires --%s.\n", required);
    for (const char *forbidden : {"force", "gui", "run", "pack-only", "test", "pre-pack", "pre-place",
                                  "pre-route", "post-route", "on-failure", "ignore-loops", "ignore-rel-clk", "sdf", "sdf-cvc", "freq"})
        if (vm.count(forbidden))
            log_error("--timing-only is incompatible with --%s.\n", forbidden);

    // validate() runs before even the logfile is opened. Check resolved paths
    // and refuse existing outputs, including symlinks/hardlinks to old evidence.
    std::vector<std::filesystem::path> inputs, outputs;
    for (const char *option : {"json", "sdc", "chipdb"})
        if (vm.count(option))
            inputs.push_back(std::filesystem::weakly_canonical(vm[option].as<std::string>()));
    if (vm.count("vopt"))
        for (const auto &option : vm["vopt"].as<std::vector<std::string>>())
            for (std::string key : {"modern_ff_timing=", "cst="})
                if (option.compare(0, key.size(), key) == 0)
                    inputs.push_back(std::filesystem::weakly_canonical(option.substr(key.size())));
    for (const char *option : {"write", "report", "log", "sdf"}) {
        if (!vm.count(option))
            continue;
        std::filesystem::path raw(vm[option].as<std::string>());
        auto output = std::filesystem::weakly_canonical(raw);
        if (std::filesystem::exists(raw) || std::filesystem::is_symlink(raw) ||
            std::find(inputs.begin(), inputs.end(), output) != inputs.end() ||
            std::find(outputs.begin(), outputs.end(), output) != outputs.end())
            log_error("Timing-only --%s must name a new, distinct output, not an input or existing file.\n", option);
        outputs.push_back(output);
    }
}

po::options_description HimbaechelCommandHandler::getArchOptions()
{
    std::string all_uarches = HimbaechelArch::list();
    std::string uarch_help = stringf("himbächel micro-arch to use (available: %s)", all_uarches.c_str());
    po::options_description specific("Architecture specific options");
    specific.add_options()("device", po::value<std::string>(), "name of device to use");
    specific.add_options()("chipdb", po::value<std::string>(), "override path to chip database file");
    specific.add_options()("list-uarch", "list included uarches");
    specific.add_options()("timing-only", "analyse a complete routed checkpoint with the router's fast-pip delay model, without implementation passes");
    specific.add_options()("timing-only-unconstrained-clock", po::value<std::vector<std::string>>(),
                           "explicit packed clock net retaining its original absent waveform constraint");
    specific.add_options()("vopt,o", po::value<std::vector<std::string>>(),
                           "options to pass to the himbächel uarch (use help as argument to get more info)");

    return specific;
}

void HimbaechelCommandHandler::setupArchContext(Context *ctx)
{
    ctx->uarch->setupArchContext();
    if (vm.count("timing-only"))
        ctx->settings[ctx->id("frontend/preserveCheckpoint")] = true;
}

void HimbaechelCommandHandler::customAfterLoad(Context *ctx)
{
    if (!vm.count("timing-only"))
        return;
    pool<IdString> unconstrained_clocks;
    if (vm.count("timing-only-unconstrained-clock"))
        for (const auto &name : vm["timing-only-unconstrained-clock"].as<std::vector<std::string>>())
            if (!unconstrained_clocks.insert(ctx->id(name)).second)
                log_error("Duplicate unconstrained timing-only clock %s.\n", name.c_str());
    if (!ctx->uarch->prepareTimingOnly(unconstrained_clocks))
        log_error("Timing-only checkpoint reconstruction is unsupported by this uarch.\n");
}

void HimbaechelCommandHandler::customBitstream(Context *ctx)
{
    if (!vm.count("timing-only"))
        return;
    // Arch::route() reports timing while fast pip delays are enabled. Match
    // that source model explicitly: ordinary JSON binding otherwise accumulates
    // RC bookkeeping in serialization order, unlike the original router run.
    // This changes delay lookup only, never placement/routing. Full RC reload
    // is not qualified by this path; do not mix it into the self-controls.
    ctx->set_fast_pip_delays(true);
    ctx->settings[ctx->id("timing.reanalysis.pip_model")] = std::string("HIMBAECHEL_ROUTER_FAST_PIP_DELAYS");
    // Construct a fresh analyser; writing a JSON report alone does not do this.
    timing_analysis(ctx, true, true, true, true, true);
    ctx->settings[ctx->id("timing.reanalysis.verdict")] = std::string("SOURCE_MODEL_ONLY_NOT_TIMING_SIGNOFF");
    log_info("Timing-only analysis complete; no implementation passes were run.\n");
}

std::unique_ptr<Context> HimbaechelCommandHandler::createContext(dict<std::string, Property> &values)
{
    ArchArgs chipArgs;
    if (values.find("arch.name") != values.end()) {
        std::string arch_name = values["arch.name"].as_string();
        if (arch_name != "himbaechel")
            log_error("Unsupported architecture '%s'.\n", arch_name.c_str());
    }
    if (vm.count("list-uarch")) {
        std::string uarches = HimbaechelArch::list();
        log_info("Supported uarches: %s\n", uarches.c_str());
        exit(0);
    }
    if (!vm.count("device"))
        log_error("device must be specified\n");
    chipArgs.device = vm["device"].as<std::string>();

    if (vm.count("chipdb")) {
        chipArgs.chipdb_override = vm["chipdb"].as<std::string>();
    }

    if (vm.count("vopt")) {
        std::vector<std::string> options = vm["vopt"].as<std::vector<std::string>>();
        chipArgs.vopts.push_back("vopt");
        for (const auto &opt : options)
            chipArgs.vopts.push_back("--" + opt);
    }
    auto ctx = std::unique_ptr<Context>(new Context(chipArgs));
    if (vm.count("gui"))
        ctx->uarch->with_gui = true;
    ctx->uarch->init(ctx.get());
    ctx->late_init();
    return ctx;
}

int main(int argc, char *argv[])
{
    HimbaechelCommandHandler handler(argc, argv);
    return handler.exec();
}
