#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pm {

// Minimal RFC 6762/6763 responder: answers PTR/SRV/TXT/A for registered services on every
// IPv4 interface. Shares UDP 5353 with Bonjour and the Windows DNS client via SO_REUSEADDR.
class MdnsResponder {
public:
    struct Service {
        std::string instance;  // single label, e.g. "PhoneMirror"
        std::string type;      // e.g. "_airplay._tcp.local."
        uint16_t port = 0;
        std::vector<uint8_t> txt;  // raw TXT rdata (length-prefixed strings)
    };

    static MdnsResponder& instance();

    int add(Service service);
    void remove(int id);
    void shutdown();  // sends goodbyes; call before process exit

private:
    MdnsResponder() = default;
    ~MdnsResponder();

    struct Interface {
        int sock;
        uint32_t addr;  // network byte order
    };
    struct Registration {
        int id;
        Service service;
        int announcements_left;
        uint64_t next_announce_ms;
    };
    struct Question {
        std::string name;
        uint16_t type;
        bool unicast;
    };

    void ensure_running();
    void run();
    void open_sockets();
    void close_sockets();
    void handle_packet(const Interface& iface);
    void announce_due(uint64_t now_ms);
    void send_goodbye(const Service& service);

    std::mutex mutex_;
    std::vector<Registration> services_;
    std::vector<Interface> interfaces_;
    std::string host_;
    int next_id_ = 1;
    std::thread thread_;
    std::atomic<bool> running_ = false;
    std::atomic<bool> rebuild_ = false;
    void* change_handle_ = nullptr;
};

}  // namespace pm
