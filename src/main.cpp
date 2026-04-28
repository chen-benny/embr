#include <iostream>
#include <string>

#include "cli/pull_cli.hpp"
#include "cli/push_cli.hpp"
#include "cli/tracker_cli.hpp"
#include "cli/trust_cli.hpp"

namespace {

void print_usage() {
    std::cerr << "Usage:\n"
              << "  embr push <file>        [--port PORT] [--tracker URL] [--ip IP]\n"
              << "  embr pull <token-or-ip> [--port PORT] [--tracker URL] [--out PATH]\n"
              << "  embr tracker            [--bind ADDR] [--port PORT] [--ttl MINUTES]\n";
}

}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const std::string verb = argv[1];

    if (verb == "push") { return run_push_cli(argc - 1, argv + 1); }
    if (verb == "pull") { return run_pull_cli(argc - 1, argv + 1); }
    if (verb == "tracker") { return run_tracker_cli(argc - 1, argv + 1); }
    if (verb == "trust") { return run_trust_cli(argc - 1, argv + 1); }

    std::cerr << "Unknown command: " << verb << "\n";
    print_usage();
    return 1;
}