//
// Created by benny on 4/25/26.
//

#include "tracker_client.hpp"
#include "util/json_parser.hpp"
#include "util/constants.hpp"
#include <httplib.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

struct ParsedUrl {
    std::string tracker_host; // domain or ip of tracker
    int tracker_port;
};

// Parse "http://host:port" -> {tracker_host, tracker_port}
//       "host" -> {tracker_host, default_port=10009}
ParsedUrl parse_url(const std::string& tracker_url) {
    const std::string prefix = "http://"; // no support TLS yet
    size_t start = 0;
    if (tracker_url.substr(0, prefix.size()) == prefix) {
        start = prefix.size();
    }
    const size_t colon_pos = tracker_url.rfind(':');
    if (colon_pos == std::string::npos || colon_pos < start) {
        return { tracker_url.substr(start), TRACKER_PORT };
    }
    return {
        tracker_url.substr(start, colon_pos - start),
        std::stoi(tracker_url.substr(colon_pos + 1))
    };
}

}

// Sender registers token with (sender_ip, sender_port) at tracker
void tracker_register(const std::string& tracker_url,
                      const std::string& token,
                      uint16_t sender_port) {
    auto [tracker_host, tracker_port] = parse_url(tracker_url);
    httplib::Client client(tracker_host, tracker_port);
    client.set_connection_timeout(10);
    client.set_read_timeout(10);

    const std::string body = "{\"token\":\"" + token + "\","
                             "\"sender_port\":" + std::to_string(sender_port) + "}";
    auto result = client.Post("/register", body, "application/json");
    if (!result) {
        throw std::runtime_error("tracker_register: connection failed to " + tracker_url);
    }
    if (result->status != 200) {
        throw std::runtime_error("tracker_register: HTTP " +
                                 std::to_string(result->status) +
                                 " from " + tracker_url);
    }
}

// Receiver asks tracker to resolve token, gets sender_ip, sender_port
std::pair<std::string, uint16_t> tracker_resolve(const std::string& tracker_url,
                                                 const std::string& token) {
    auto [tracker_host, tracker_port] = parse_url(tracker_url);
    httplib::Client client(tracker_host, tracker_port);
    client.set_connection_timeout(10);
    client.set_read_timeout(10);

    auto result = client.Get("/resolve/" + token);
    if (!result) {
        throw std::runtime_error("tracker_resolve: connection failed to " + tracker_url);
    }
    if (result->status == 404) {
        throw std::runtime_error("tracker_resolve: token not found: " + token);
    }
    if (result->status != 200) {
        throw std::runtime_error("tracker_resolve: HTTP "  +
                                 std::to_string(result->status) +
                                 " from " + tracker_url);
    }

    const std::string sender_ip = json_get_str(result->body, "sender_ip");
    const uint16_t sender_port = json_get_u16(result->body, "sender_port");
    if (sender_ip.empty() || sender_port == 0) {
        throw std::runtime_error("tracker_resolve: malformed response: " + result->body);
    }
    return {sender_ip, sender_port};
}

// Sender informs tracker to unregister its token
void tracker_unregister(const std::string& tracker_url,
                        const std::string& token) noexcept {
    try {
        auto [tracker_host, tracker_port] = parse_url(tracker_url);
        httplib::Client client(tracker_host, tracker_port);
        client.set_connection_timeout(10);
        client.set_read_timeout(10);
        client.Post("/unregister/" + token, "", "application/json");
    } catch (...) {
        // best effort unregister
    }
}