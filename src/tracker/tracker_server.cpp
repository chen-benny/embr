//
// Created by benny on 4/26/26.
//

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "token_store.hpp"
#include "tracker_handlers.hpp"
#include "tracker_server.hpp"
#include <httplib.h>

namespace {

// global server ptr for signal handler, set once
httplib::Server* g_server = nullptr;

void signal_handler(int) {
    if (g_server) { g_server->stop(); }
}

}

void run_tracker_server(const tracker_config& config) {
    TokenStore store(config.ttl);
    httplib::Server server;
    g_server = &server;

    ::signal(SIGINT, signal_handler);
    ::signal(SIGTERM, signal_handler);

    // background sweeper: evict expired token every 60s
    std::jthread sweeper([&store](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            if (stop_token.stop_requested()) { break; }
            const size_t evicted = store.sweep_expired();
            if (evicted > 0) {
                std::cout << "[tracker] sweep evicted " << evicted << " records\n";
            }
        }
    });

    // POST /register
    server.Post("/register", [&store](const httplib::Request& req,
                                      httplib::Response& res) {
        handle_register(req, res, store);
    });

    // GET /resolve/:token
    server.Get("/resolve/:token", [&store](const httplib::Request& req,
                                           httplib::Response& res) {
        handle_resolve(req, res, store);
    });

    // POST /unregister/:token
    server.Post("/unregister/:token", [&store](const httplib::Request& req,
                                               httplib::Response& res) {
        handle_unregister(req, res, store);
    });

    std::cout << "[tracker] listening on "
              << config.bind_addr << ":" << config.bind_port
              << "(TTL=" << config.ttl.count() << "min)\n";

    if (!server.listen(config.bind_addr, config.bind_port)) {
        std::cerr << "[tracker] failed to bind "
                  << config.bind_addr << ":" << config.bind_port << "\n";
    }

    // server.listen() returns after server.stop() is called by signal handler
    sweeper.request_stop();
    std::cout << "[tracker] shutdown complete\n";
}