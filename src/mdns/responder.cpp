#include "mdns/responder.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <span>
#include <string_view>

#include "app/log.h"
#pragma warning(push, 0)
#include "mdns/mdns.h"
#pragma warning(pop)

namespace pm {
namespace {

constexpr uint16_t kTypeA = 1;
constexpr uint16_t kTypePtr = 12;
constexpr uint16_t kTypeTxt = 16;
constexpr uint16_t kTypeSrv = 33;
constexpr uint16_t kTypeAny = 255;
constexpr uint16_t kClassIn = 1;
constexpr uint16_t kCacheFlush = 0x8000;
constexpr uint32_t kTtlShared = 4500;
constexpr uint32_t kTtlUnique = 120;
constexpr uint32_t kTtlLegacyMax = 10;
constexpr int kAnnouncements = 3;
constexpr std::string_view kServicesName = "_services._dns-sd._udp.local.";

using Service = MdnsResponder::Service;

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
}

void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    put16(out, static_cast<uint16_t>(v >> 16));
    put16(out, static_cast<uint16_t>(v));
}

// "label.label.local." -> DNS wire labels. Callers keep '.' out of instance labels.
std::vector<uint8_t> encode_name(std::string_view dotted) {
    std::vector<uint8_t> out;
    size_t start = 0;
    while (start < dotted.size()) {
        size_t dot = dotted.find('.', start);
        if (dot == std::string_view::npos) dot = dotted.size();
        size_t len = std::min<size_t>(dot - start, 63);
        if (len) {
            out.push_back(static_cast<uint8_t>(len));
            out.insert(out.end(), dotted.begin() + start, dotted.begin() + start + len);
        }
        start = dot + 1;
    }
    out.push_back(0);
    return out;
}

struct Record {
    std::string name;
    uint16_t type;
    bool unique;
    uint32_t ttl;
    std::vector<uint8_t> rdata;

    bool operator==(const Record& o) const { return type == o.type && iequals(name, o.name) && rdata == o.rdata; }
};

std::string full_name(const Service& s) { return s.instance + "." + s.type; }

Record ptr_record(std::string_view name, std::string_view target) {
    return {std::string(name), kTypePtr, false, kTtlShared, encode_name(target)};
}

Record srv_record(const Service& s, const std::string& host) {
    std::vector<uint8_t> rdata;
    put16(rdata, 0);  // priority
    put16(rdata, 0);  // weight
    put16(rdata, s.port);
    std::vector<uint8_t> target = encode_name(host);
    rdata.insert(rdata.end(), target.begin(), target.end());
    return {full_name(s), kTypeSrv, true, kTtlUnique, std::move(rdata)};
}

Record txt_record(const Service& s) {
    return {full_name(s), kTypeTxt, true, kTtlUnique, s.txt.empty() ? std::vector<uint8_t>{0} : s.txt};
}

Record a_record(const std::string& host, uint32_t addr) {
    std::vector<uint8_t> rdata(4);
    std::memcpy(rdata.data(), &addr, 4);
    return {host, kTypeA, true, kTtlUnique, std::move(rdata)};
}

void add_once(std::vector<Record>& list, Record r) {
    if (std::find(list.begin(), list.end(), r) == list.end()) list.push_back(std::move(r));
}

std::vector<uint8_t> build_packet(uint16_t id, std::span<const std::pair<std::string, uint16_t>> echo,
                                  const std::vector<Record>& answers, const std::vector<Record>& additional,
                                  bool legacy, bool goodbye) {
    std::vector<uint8_t> p;
    p.reserve(1024);
    put16(p, id);
    put16(p, 0x8400);  // response, authoritative
    put16(p, static_cast<uint16_t>(echo.size()));
    put16(p, static_cast<uint16_t>(answers.size()));
    put16(p, 0);
    put16(p, static_cast<uint16_t>(additional.size()));
    for (const auto& [name, type] : echo) {
        std::vector<uint8_t> n = encode_name(name);
        p.insert(p.end(), n.begin(), n.end());
        put16(p, type);
        put16(p, kClassIn);
    }
    auto emit = [&](const Record& r) {
        std::vector<uint8_t> n = encode_name(r.name);
        p.insert(p.end(), n.begin(), n.end());
        put16(p, r.type);
        put16(p, kClassIn | (r.unique && !legacy ? kCacheFlush : 0));
        put32(p, goodbye ? 0 : legacy ? std::min(r.ttl, kTtlLegacyMax) : r.ttl);
        put16(p, static_cast<uint16_t>(r.rdata.size()));
        p.insert(p.end(), r.rdata.begin(), r.rdata.end());
    };
    for (const Record& r : answers) emit(r);
    for (const Record& r : additional) emit(r);
    return p;
}

std::vector<Record> service_records(const Service& s, const std::string& host, uint32_t addr) {
    return {ptr_record(s.type, full_name(s)), srv_record(s, host), txt_record(s), a_record(host, addr)};
}

sockaddr_in multicast_group() {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(MDNS_PORT);
    to.sin_addr.s_addr = htonl((224u << 24) | 251u);
    return to;
}

void send_to(int sock, const std::vector<uint8_t>& packet, const sockaddr_in& to) {
    sendto(static_cast<SOCKET>(sock), reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

std::string ip_string(uint32_t addr) {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr in;
    in.s_addr = addr;
    inet_ntop(AF_INET, &in, buf, sizeof(buf));
    return buf;
}

struct Incoming {
    std::vector<std::pair<std::string, uint16_t>> questions;
    bool unicast = false;
    uint16_t query_id = 0;
    sockaddr_in from{};
    bool from_ipv4 = false;
};

int on_record(int, const sockaddr* from, size_t addrlen, mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
              uint16_t rclass, uint32_t, const void* data, size_t size, size_t name_offset, size_t, size_t, size_t,
              void* user) {
    if (entry != MDNS_ENTRYTYPE_QUESTION) return 0;
    auto* in = static_cast<Incoming*>(user);
    char buf[256];
    size_t offset = name_offset;
    mdns_string_t name = mdns_string_extract(data, size, &offset, buf, sizeof(buf));
    std::string n(name.str, name.length);
    if (n.empty() || n.back() != '.') n += '.';
    in->questions.emplace_back(std::move(n), rtype);
    in->unicast |= (rclass & MDNS_UNICAST_RESPONSE) != 0;
    in->query_id = query_id;
    if (from->sa_family == AF_INET && addrlen >= sizeof(sockaddr_in)) {
        std::memcpy(&in->from, from, sizeof(sockaddr_in));
        in->from_ipv4 = true;
    }
    return 0;
}

void CALLBACK on_address_change(PVOID context, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
    static_cast<std::atomic<bool>*>(context)->store(true);
}

}  // namespace

MdnsResponder& MdnsResponder::instance() {
    static MdnsResponder responder;
    return responder;
}

MdnsResponder::~MdnsResponder() { shutdown(); }

void MdnsResponder::shutdown() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (change_handle_) CancelMibChangeNotify2(change_handle_);
    change_handle_ = nullptr;
    std::lock_guard lock(mutex_);
    for (const Registration& r : services_) send_goodbye(r.service);
    services_.clear();
    close_sockets();
    WSACleanup();
}

int MdnsResponder::add(Service service) {
    ensure_running();
    std::lock_guard lock(mutex_);
    int id = next_id_++;
    log::info("mDNS: registering \"{}\" {} port {}", service.instance, service.type, service.port);
    services_.push_back({id, std::move(service), kAnnouncements, 0});
    return id;
}

void MdnsResponder::remove(int id) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(services_.begin(), services_.end(), [id](const Registration& r) { return r.id == id; });
    if (it == services_.end()) return;
    send_goodbye(it->service);
    services_.erase(it);
}

void MdnsResponder::ensure_running() {
    if (running_.exchange(true)) return;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    char computer[256] = {};
    DWORD len = sizeof(computer);
    GetComputerNameExA(ComputerNameDnsHostname, computer, &len);
    // Bonjour already owns "<computer>.local", so publish our own host name next to it, restricted to
    // valid hostname characters (Windows allows '_' in computer names).
    std::string label = len ? computer : "PhoneMirror";
    for (char& c : label) {
        if (!isalnum(static_cast<unsigned char>(c))) c = '-';
    }
    host_ = label + "-pm.local.";

    HANDLE handle = nullptr;
    if (NotifyUnicastIpAddressChange(AF_INET, on_address_change, &rebuild_, FALSE, &handle) == NO_ERROR) {
        change_handle_ = handle;
    }
    thread_ = std::thread([this] { run(); });
}

void MdnsResponder::open_sockets() {
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (ret != NO_ERROR) {
        log::error("mDNS: GetAdaptersAddresses failed: {}", ret);
        return;
    }
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL ||
            (a->Flags & IP_ADAPTER_NO_MULTICAST)) {
            continue;
        }
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            sockaddr_in sin = *reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            sin.sin_port = htons(MDNS_PORT);
            int sock = mdns_socket_open_ipv4(&sin);
            if (sock < 0) {
                log::error("mDNS: cannot bind 5353 on {} ({}): WSA {}", ip_string(sin.sin_addr.s_addr),
                           log::utf8(a->FriendlyName), WSAGetLastError());
                continue;
            }
            interfaces_.push_back({sock, sin.sin_addr.s_addr});
            log::info("mDNS: listening on {} ({})", ip_string(sin.sin_addr.s_addr), log::utf8(a->FriendlyName));
        }
    }
}

void MdnsResponder::close_sockets() {
    for (const Interface& i : interfaces_) closesocket(static_cast<SOCKET>(i.sock));
    interfaces_.clear();
}

void MdnsResponder::run() {
    {
        std::lock_guard lock(mutex_);
        open_sockets();
    }
    while (running_) {
        if (rebuild_.exchange(false)) {
            std::lock_guard lock(mutex_);
            log::info("mDNS: network change, rebinding");
            close_sockets();
            open_sockets();
            for (Registration& r : services_) {
                r.announcements_left = kAnnouncements;
                r.next_announce_ms = 0;
            }
        }

        fd_set readable;
        FD_ZERO(&readable);
        std::vector<Interface> ifaces;
        {
            std::lock_guard lock(mutex_);
            ifaces = interfaces_;
        }
        for (const Interface& i : ifaces) FD_SET(static_cast<SOCKET>(i.sock), &readable);
        timeval timeout{0, 200 * 1000};
        if (ifaces.empty()) {
            Sleep(200);
        } else if (select(0, &readable, nullptr, nullptr, &timeout) > 0) {
            for (const Interface& i : ifaces) {
                if (FD_ISSET(static_cast<SOCKET>(i.sock), &readable)) handle_packet(i);
            }
        }
        announce_due(GetTickCount64());
    }
}

void MdnsResponder::handle_packet(const Interface& iface) {
    alignas(8) uint8_t buffer[4096];
    Incoming in;
    mdns_socket_listen(iface.sock, buffer, sizeof(buffer), on_record, &in);
    if (in.questions.empty() || !in.from_ipv4) return;

    std::lock_guard lock(mutex_);
    std::vector<Record> answers;
    std::vector<Record> additional;
    for (const auto& [name, type] : in.questions) {
        bool any = type == kTypeAny;
        if (iequals(name, kServicesName) && (any || type == kTypePtr)) {
            for (const Registration& r : services_) add_once(answers, ptr_record(kServicesName, r.service.type));
            continue;
        }
        for (const Registration& r : services_) {
            const Service& s = r.service;
            if (iequals(name, s.type) && (any || type == kTypePtr)) {
                add_once(answers, ptr_record(s.type, full_name(s)));
                add_once(additional, srv_record(s, host_));
                add_once(additional, txt_record(s));
                add_once(additional, a_record(host_, iface.addr));
            } else if (iequals(name, full_name(s))) {
                if (any || type == kTypeSrv) {
                    add_once(answers, srv_record(s, host_));
                    add_once(additional, a_record(host_, iface.addr));
                }
                if (any || type == kTypeTxt) add_once(answers, txt_record(s));
            }
        }
        if (iequals(name, host_) && (any || type == kTypeA)) add_once(answers, a_record(host_, iface.addr));
    }
    if (answers.empty()) return;
    std::erase_if(additional, [&](const Record& r) { return std::find(answers.begin(), answers.end(), r) != answers.end(); });

    // Legacy unicast (source port != 5353) must echo the id and questions; QU questions get a direct reply.
    bool legacy = ntohs(in.from.sin_port) != MDNS_PORT;
    std::vector<uint8_t> packet =
        build_packet(legacy ? in.query_id : 0, legacy ? std::span(in.questions) : std::span<const std::pair<std::string, uint16_t>>(),
                     answers, additional, legacy, false);
    send_to(iface.sock, packet, legacy || in.unicast ? in.from : multicast_group());
}

void MdnsResponder::announce_due(uint64_t now) {
    std::lock_guard lock(mutex_);
    for (Registration& r : services_) {
        if (r.announcements_left <= 0 || now < r.next_announce_ms) continue;
        for (const Interface& i : interfaces_) {
            send_to(i.sock, build_packet(0, {}, service_records(r.service, host_, i.addr), {}, false, false),
                    multicast_group());
        }
        // RFC 6762 §8.3: at least two announcements, intervals doubling from one second.
        r.next_announce_ms = now + (1000ull << (kAnnouncements - r.announcements_left));
        --r.announcements_left;
    }
}

void MdnsResponder::send_goodbye(const Service& service) {
    for (const Interface& i : interfaces_) {
        send_to(i.sock, build_packet(0, {}, service_records(service, host_, i.addr), {}, false, true),
                multicast_group());
    }
}

}  // namespace pm
