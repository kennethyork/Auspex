// auspex-probe — exercises the native backend layer against the real services.
//
// Purpose: prove the Python-free core works before any GTK code exists.
//   auspex-probe            config + ollama reachability + model list
//   auspex-probe ask "..."  one-shot generation through the configured model
#include <iostream>
#include <string>
#include <vector>

#include "auspex/commands.hpp"
#include "auspex/config.hpp"
#include "auspex/ollama_client.hpp"

namespace {

void print_config(const auspex::Config& c) {
    std::cout << "config          : " << auspex::Config::default_path() << "\n"
              << "  terminal      : " << c.terminal << "\n"
              << "  launcher      : " << c.launcher << "\n"
              << "  ollama_model  : " << c.ollama_model << "\n"
              << "  ollama_endpt  : " << c.ollama_endpoint << "\n"
              << "  whisper_endpt : " << c.whisper_endpoint << "\n"
              << "  panel_height  : " << c.panel_height << "\n"
              << "  workspaces    : " << c.workspace_count << "\n"
              << "  tts_command   : " << (c.tts_command.empty() ? "(unset)" : c.tts_command)
              << "\n\n";
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    const auspex::Config cfg = auspex::Config::load();
    auspex::OllamaClient ollama(cfg);

    // Interpret an utterance as a desktop command WITHOUT executing it, so the
    // model's behaviour can be checked without anything actually happening.
    if (args.size() >= 2 && args[0] == "command") {
        std::string utterance = args[1];
        for (std::size_t i = 2; i < args.size(); ++i) utterance += " " + args[i];

        auspex::CommandContext context = auspex::gather_context(cfg);
        // run_crew takes the user's own words as its argument, never the
        // model's paraphrase, so the transcript has to reach execute_action.
        context.utterance = utterance;
        const auto reply = ollama.generate(
            cfg.ollama_model, auspex::build_command_prompt(utterance, context),
            auspex::GenerateOptions{.num_predict = 200,
                                    .temperature = 0.0,
                                    .json = true,
                                    .disable_thinking = true});
        if (!reply) {
            std::cerr << "auspex-probe: no answer from ollama\n";
            return 1;
        }

        std::cout << "utterance : " << utterance << "\n"
                  << "raw       : " << reply->response << "\n";

        const auto parsed = auspex::parse_action(reply->response, context);
        if (!parsed.action) {
            std::cout << "REJECTED  : " << parsed.error << "\n";
            return 1;
        }
        std::cout << "action    : " << auspex::to_string(parsed.action->kind) << "\n"
                  << "target    : " << parsed.action->target << "\n"
                  << "number    : " << parsed.action->number << "\n";
        return 0;
    }

    if (args.size() >= 2 && args[0] == "ask") {
        std::string prompt = args[1];
        for (std::size_t i = 2; i < args.size(); ++i) prompt += " " + args[i];

        const auto reply = ollama.generate(cfg.ollama_model, prompt);
        if (!reply) {
            std::cerr << "auspex-probe: generation failed (is ollama running?)\n";
            return 1;
        }
        if (!reply->thinking.empty()) {
            std::cerr << "[thinking] " << reply->thinking << "\n\n";
        }
        if (reply->response.empty()) {
            std::cerr << "auspex-probe: model completed with no answer (done_reason="
                      << (reply->done_reason.empty() ? "?" : reply->done_reason) << ")\n";
            return 1;
        }
        std::cout << reply->response << "\n";
        return 0;
    }

    print_config(cfg);

    const auto ver = ollama.version();
    std::cout << "ollama version  : " << (ver ? *ver : "UNREACHABLE") << "\n";

    if (ver) {
        const auto models = ollama.list_models();
        std::cout << "models pulled   : " << models.size() << "\n";
        for (const auto& m : models) {
            std::cout << "  " << (m == cfg.ollama_model ? "* " : "  ") << m << "\n";
        }
    }

    std::cout << "\nprobing configured model...\n";
    const auto status = ollama.check_status([](const auspex::OracleStatus& s) {
        std::cout << "  [" << s.label() << " " << s.progress << "%] " << s.message << "\n";
    });

    return status.state == auspex::OracleState::Running ? 0 : 1;
}
