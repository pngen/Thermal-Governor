// Thermal Governor — command line interface.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "session.hpp"
#include "thermal_governor/thermal_governor.hpp"
#include "thermal_governor/wire.hpp"

using thermal_governor::wire::CommandRequest;

namespace {

void print_usage() {
    std::printf(
        "thermal-governor CLI %s\n"
        "\n"
        "The CLI is a thin client. It connects to a running coordinator and\n"
        "issues commands over the framed transport, so every mutation travels\n"
        "exactly the same authority path a library caller would use.\n"
        "\n"
        "Usage: thermal_governor_cli --port <port> [--host <host>] <command> [args]\n"
        "\n"
        "Read commands:\n"
        "  status                     coordinator and topology summary\n"
        "  devices                    list governed devices\n"
        "  device <id>                one device\n"
        "  domains                    list thermal domains\n"
        "  domain <id>                thermal envelope of a domain\n"
        "  temperature <id>           current temperature evidence of a domain\n"
        "  headroom <id>              raw, margin and effective headroom\n"
        "  throttle <id>              throttle classification\n"
        "  recovery <id>              recovery eligibility assessment\n"
        "  policy                     active thermal policy\n"
        "  evaluate <id>              evaluate a thermal domain\n"
        "  explain <id>               deterministic structured explanation\n"
        "  admission <domain> <workload> <profile>\n"
        "  actions                    recorded mitigation actions\n"
        "  history                    durable thermal state transitions\n"
        "  snapshot-info              immutable snapshot counters\n"
        "\n"
        "Mutation commands:\n"
        "  set-thresholds <warning> <derating> <critical> <recovery> <band>\n"
        "                 <safety> <uncertainty> <recovery-margin> <generation>\n"
        "                 <max-age-ms> <required-samples> <min-span-ms>\n"
        "  register-device <id> <gen> <node> <node-gen> <rack> <rack-gen> <label>\n"
        "                  <temperature-cap> <throttle-cap> <limit-cap> <provenance>\n"
        "  register-domain <id> <gen> <type> <provenance> <aggregation> <label>\n"
        "                  <policy-id> [<member>...]\n"
        "  register-coupling <id> <source> <destination> <type> <provenance> <weight>\n"
        "  authorize-derating <domain> <intent>\n"
        "  authorize-mitigation <domain> <intent>\n"
        "  dispatch <action-id> | cancel <action-id>\n"
        "  bump-domain-generation <domain> | bump-device-generation <device>\n"
        "  worker-death <worker-id> <boot-id>\n"
        "  persist | shutdown\n",
        std::string(thermal_governor::version_string()).c_str());
}

}  // namespace

int main(int argc, char** argv) {
    using namespace thermal_governor;
    using namespace thermal_governor::tools;

    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        }
        if (argument == "--host" && i + 1 < argc) {
            host = argv[++i];
            continue;
        }
        if (argument == "--port" && i + 1 < argc) {
            std::uint64_t parsed = 0;
            if (!parse_u64(argv[++i], parsed) || parsed > 65535) {
                std::fprintf(stderr, "malformed port\n");
                return 2;
            }
            port = static_cast<std::uint16_t>(parsed);
            continue;
        }
        positional.push_back(argument);
    }

    if (positional.empty()) {
        print_usage();
        return 2;
    }
    if (port == 0) {
        std::fprintf(stderr, "--port is required: the CLI is a client of a running coordinator\n");
        return 2;
    }

    auto connected = TcpSocket::connect_to(host, port);
    if (!connected.has_value()) {
        std::fprintf(stderr, "connect failed: %s\n", connected.error().render().c_str());
        return 1;
    }
    TcpSocket socket = std::move(connected.value());

    CommandRequest request;
    request.verb = positional.front();
    for (std::size_t i = 1; i < positional.size(); ++i) {
        request.arguments.push_back(positional[i]);
    }

    auto bytes = wire::encode_command(request);
    auto status = send_bytes(socket, MessageKind::QUERY_REQUEST, 0, bytes);
    if (!status.ok()) {
        std::fprintf(stderr, "send failed: %s\n", status.error().render().c_str());
        return 1;
    }
    (void)send_end(socket);

    if (request.verb == "shutdown") {
        // The coordinator closes the connection as it stops; that is the
        // expected outcome of a shutdown request, not a transport failure.
        std::printf("shutdown requested\n");
        return 0;
    }

    std::vector<Frame> frames;
    status = read_group(socket, frames);
    if (!status.ok()) {
        std::fprintf(stderr, "receive failed: %s\n", status.error().render().c_str());
        return 1;
    }

    int exit_code = 0;
    for (const auto& frame : frames) {
        if (frame.kind != MessageKind::QUERY_RESPONSE) {
            continue;
        }
        auto response = wire::decode_response(frame.payload.data(), frame.payload.size());
        if (!response.has_value()) {
            std::fprintf(stderr, "malformed response\n");
            return 1;
        }
        if (!response.value().status.ok()) {
            std::printf("ERROR %s %s\n",
                        std::string(to_string(response.value().status.code)).c_str(),
                        response.value().status.detail.c_str());
            exit_code = 3;
            continue;
        }
        std::fputs(response.value().payload.c_str(), stdout);
        if (!response.value().payload.empty() && response.value().payload.back() != '\n') {
            std::fputc('\n', stdout);
        }
    }
    std::fflush(stdout);
    return exit_code;
}
