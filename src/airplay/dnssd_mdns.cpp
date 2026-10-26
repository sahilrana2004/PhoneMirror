// Implements UxPlay's dnssd.h on the embedded mDNS responder instead of Bonjour's dnssd.dll.
// TXT contents mirror upstream lib/dnssd.c (v1.73.7) so iOS sees an identical receiver.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "mdns/responder.h"

extern "C" {
#include "dnssd.h"
#include "dnssdint.h"
#include "utils.h"
}

struct dnssd_s {
    std::string name;
    std::string hw_addr;
    const char* pk = "";
    uint32_t features1 = 0;
    uint32_t features2 = 0;
    unsigned char pin_pw = 0;
    std::vector<uint8_t> raop_txt;
    std::vector<uint8_t> airplay_txt;
    int raop_id = 0;
    int airplay_id = 0;
};

namespace {

// Same semantics as Bonjour's TXTRecordSetValue: an existing key is removed, then the pair is appended.
void txt_add(std::vector<uint8_t>& txt, std::string_view key, std::string_view value) {
    size_t len = key.size() + 1 + value.size();
    if (len > 255) return;
    for (size_t i = 0; i < txt.size();) {
        size_t entry = txt[i];
        std::string_view existing(reinterpret_cast<const char*>(&txt[i + 1]), entry);
        if (existing.size() > key.size() && existing.compare(0, key.size(), key) == 0 && existing[key.size()] == '=') {
            txt.erase(txt.begin() + static_cast<ptrdiff_t>(i), txt.begin() + static_cast<ptrdiff_t>(i + 1 + entry));
        } else {
            i += 1 + entry;
        }
    }
    txt.push_back(static_cast<uint8_t>(len));
    txt.insert(txt.end(), key.begin(), key.end());
    txt.push_back('=');
    txt.insert(txt.end(), value.begin(), value.end());
}

std::string features_string(const dnssd_t* d) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%X,0x%X", d->features1, d->features2);
    return buf;
}

// Instance names are single DNS labels; keep dots out so they encode as one label.
std::string label_safe(std::string name) {
    for (char& c : name) {
        if (c == '.') c = '-';
    }
    return name;
}

}  // namespace

extern "C" {

dnssd_t* dnssd_init(const char* name, int name_len, const char* hw_addr, int hw_addr_len, int* error,
                    unsigned char pin_pw) {
    if (error) *error = DNSSD_ERROR_NOERROR;
    auto* d = new dnssd_t;
    d->name.assign(name, static_cast<size_t>(name_len));
    d->hw_addr.assign(hw_addr, static_cast<size_t>(hw_addr_len));
    d->features1 = static_cast<uint32_t>(std::strtoul(FEATURES_1, nullptr, 16));
    d->features2 = static_cast<uint32_t>(std::strtoul(FEATURES_2, nullptr, 16));
    d->pin_pw = pin_pw;
    return d;
}

void dnssd_destroy(dnssd_t* dnssd) {
    if (!dnssd) return;
    dnssd_unregister_raop(dnssd);
    dnssd_unregister_airplay(dnssd);
    delete dnssd;
}

int dnssd_register_raop(dnssd_t* d, unsigned short port) {
    std::string features = features_string(d);
    std::vector<uint8_t>& t = d->raop_txt;
    t.clear();
    txt_add(t, "ch", RAOP_CH);
    txt_add(t, "cn", RAOP_CN);
    txt_add(t, "da", RAOP_DA);
    txt_add(t, "et", RAOP_ET);
    txt_add(t, "vv", RAOP_VV);
    txt_add(t, "ft", features);
    txt_add(t, "am", GLOBAL_MODEL);
    txt_add(t, "md", RAOP_MD);
    txt_add(t, "rhd", RAOP_RHD);
    switch (d->pin_pw) {
    case 2:
    case 3:
        txt_add(t, "pw", "true");
        txt_add(t, "sf", "0x84");
        break;
    case 1:
        txt_add(t, "pw", "true");
        txt_add(t, "sf", "0x8c");
        break;
    default:
        txt_add(t, "pw", "false");
        txt_add(t, "sf", RAOP_SF);
        break;
    }
    txt_add(t, "sr", RAOP_SR);
    txt_add(t, "ss", RAOP_SS);
    txt_add(t, "sv", RAOP_SV);
    txt_add(t, "tp", RAOP_TP);
    txt_add(t, "txtvers", RAOP_TXTVERS);
    txt_add(t, "sf", RAOP_SF);  // upstream sets sf twice; the second call replaces the first
    txt_add(t, "vs", RAOP_VS);
    txt_add(t, "vn", RAOP_VN);
    txt_add(t, "pk", d->pk);

    char servname[256];
    if (utils_hwaddr_raop(servname, sizeof(servname), d->hw_addr.data(), static_cast<int>(d->hw_addr.size())) < 0) {
        return -1;
    }
    pm::MdnsResponder::Service s;
    s.instance = std::string(servname) + "@" + label_safe(d->name);
    s.type = "_raop._tcp.local.";
    s.port = port;
    s.txt = t;
    d->raop_id = pm::MdnsResponder::instance().add(std::move(s));
    return 0;
}

int dnssd_register_airplay(dnssd_t* d, unsigned short port) {
    char device_id[3 * 6 + 1];
    if (utils_hwaddr_airplay(device_id, sizeof(device_id), d->hw_addr.data(), static_cast<int>(d->hw_addr.size())) < 0) {
        return -1;
    }
    std::vector<uint8_t>& t = d->airplay_txt;
    t.clear();
    txt_add(t, "deviceid", device_id);
    txt_add(t, "features", features_string(d));
    txt_add(t, "pw", d->pin_pw ? "true" : "false");
    txt_add(t, "flags", "0x4");
    txt_add(t, "model", GLOBAL_MODEL);
    txt_add(t, "pk", d->pk);
    txt_add(t, "pi", AIRPLAY_PI);
    txt_add(t, "srcvers", AIRPLAY_SRCVERS);
    txt_add(t, "vv", AIRPLAY_VV);

    pm::MdnsResponder::Service s;
    s.instance = label_safe(d->name);
    s.type = "_airplay._tcp.local.";
    s.port = port;
    s.txt = t;
    d->airplay_id = pm::MdnsResponder::instance().add(std::move(s));
    return 0;
}

void dnssd_unregister_raop(dnssd_t* d) {
    if (!d->raop_id) return;
    pm::MdnsResponder::instance().remove(d->raop_id);
    d->raop_id = 0;
}

void dnssd_unregister_airplay(dnssd_t* d) {
    if (!d->airplay_id) return;
    pm::MdnsResponder::instance().remove(d->airplay_id);
    d->airplay_id = 0;
}

const char* dnssd_get_raop_txt(dnssd_t* d, int* length) {
    *length = static_cast<int>(d->raop_txt.size());
    return reinterpret_cast<const char*>(d->raop_txt.data());
}

const char* dnssd_get_airplay_txt(dnssd_t* d, int* length) {
    *length = static_cast<int>(d->airplay_txt.size());
    return reinterpret_cast<const char*>(d->airplay_txt.data());
}

const char* dnssd_get_name(dnssd_t* d, int* length) {
    *length = static_cast<int>(d->name.size());
    return d->name.c_str();
}

const char* dnssd_get_hw_addr(dnssd_t* d, int* length) {
    *length = static_cast<int>(d->hw_addr.size());
    return d->hw_addr.data();
}

uint64_t dnssd_get_airplay_features(dnssd_t* d) { return (static_cast<uint64_t>(d->features2) << 32) | d->features1; }

void dnssd_set_pk(dnssd_t* d, char* pk_str) { d->pk = pk_str; }

void dnssd_set_airplay_features(dnssd_t* d, int bit, int val) {
    if (bit < 0 || bit > 63 || val < 0 || val > 1) return;
    uint32_t* features = bit >= 32 ? &d->features2 : &d->features1;
    uint32_t mask = 1u << (bit % 32);
    *features = val ? (*features | mask) : (*features & ~mask);
}

}  // extern "C"
