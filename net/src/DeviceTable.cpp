#include "shunt/net/DeviceTable.h"
#include <algorithm>

namespace shunt::net {

namespace {
bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}
}

Model inferModel(const std::string& name, uint16_t statusLength, DeviceKind kind) {
    if (startsWith(name, "CDJ-3000")) return Model::CDJ3000;
    if (startsWith(name, "CDJ-2000NXS2") || startsWith(name, "CDJ-TOUR1")) return Model::NXS2;
    if (startsWith(name, "CDJ-2000nexus") || startsWith(name, "CDJ-900nexus")) return Model::Nexus;
    if (startsWith(name, "XDJ-XZ")) return Model::XZ;
    if (startsWith(name, "XDJ-AZ")) return Model::AZ;
    if (startsWith(name, "XDJ-1000") || startsWith(name, "XDJ-700")) return Model::XDJ;
    if (startsWith(name, "OPUS-QUAD")) return Model::Opus;
    if (startsWith(name, "DJM-")) return Model::Mixer;
    if (startsWith(name, "rekordbox")) return Model::Rekordbox;
    if (startsWith(name, "CDJ-2000") || startsWith(name, "CDJ-900")) return Model::PreNexus;
    if (kind == DeviceKind::Mixer) return Model::Mixer;
    if (kind == DeviceKind::Rekordbox) return Model::Rekordbox;
    switch (statusLength) {
    case 0x200: return Model::CDJ3000;
    case 0xd0: return Model::PreNexus;
    case 0xd4: return Model::Nexus;
    case 0x11b: return Model::XDJ;
    case 0x11c: case 0x124: return Model::NXS2;
    case 0x38: return Model::Mixer;
    default: return Model::Unknown;
    }
}

Capabilities capabilitiesFor(Model m) {
    Capabilities c;
    switch (m) {
    case Model::CDJ3000:
        c.sendsPrecisePosition = true; c.supportsNumbers5and6 = true; c.hasKey = true; c.hasLoops = true; break;
    case Model::NXS2: case Model::Nexus: case Model::XDJ: break;
    case Model::PreNexus: c.hasFByte = false; break;
    case Model::XZ: c.embeddedMixer = true; break;
    case Model::AZ: c.supportsNumbers5and6 = true; break;
    case Model::Opus: c.sendsBeats = false; c.opusQuadQuirks = true; c.dbserverOk = false; break;
    case Model::Mixer: c.sendsBeats = true; c.dbserverOk = false; break;
    case Model::Rekordbox: c.sendsBeats = false; c.dbserverOk = false; break;
    case Model::Unknown: c.verified = false; break;   // NXS2 capabilities, unverified badge
    }
    return c;
}

const char* modelName(Model m) {
    switch (m) {
    case Model::CDJ3000: return "CDJ-3000";
    case Model::NXS2: return "NXS2";
    case Model::Nexus: return "nexus";
    case Model::PreNexus: return "pre-nexus";
    case Model::XDJ: return "XDJ";
    case Model::XZ: return "XDJ-XZ";
    case Model::AZ: return "XDJ-AZ";
    case Model::Opus: return "Opus Quad";
    case Model::Mixer: return "mixer";
    case Model::Rekordbox: return "rekordbox";
    default: return "unknown";
    }
}

Device* DeviceTable::findMut(uint8_t number) {
    for (auto& d : devices_) if (d.number == number) return &d;
    return nullptr;
}

const Device* DeviceTable::find(uint8_t number) const {
    for (auto& d : devices_) if (d.number == number) return &d;
    return nullptr;
}

bool DeviceTable::mixerPresent() const {
    for (auto& d : devices_) if (d.kind == DeviceKind::Mixer || d.number == 0x21) return true;
    return false;
}

std::optional<Event> DeviceTable::onKeepAlive(const Event& ev) {
    if (ev.type != EventType::KeepAlive) return std::nullopt;
    const auto& k = ev.keepAlive;
    Device* d = findMut(k.number);
    std::optional<Event> out;
    if (!d) {
        devices_.push_back(Device{});
        d = &devices_.back();
        d->number = k.number;
        out = Event{};
        out->type = EventType::Device;
        out->recvTimeNs = ev.recvTimeNs;
        out->device = k.number;
        out->deviceEvent.kind = DeviceEvent::Joined;
    }
    d->kind = k.kind;
    d->name = k.name;
    d->mac = k.mac;
    d->ip = k.ip;
    d->peerCount = k.peerCount;
    d->cdj3000Compatible = k.cdj3000Compatible;
    d->lastKeepAliveNs = ev.recvTimeNs;
    const Model m = inferModel(d->name, d->statusLength, d->kind);
    if (m != d->model) {
        d->model = m;
        if (!out) {
            out = Event{};
            out->type = EventType::Device;
            out->recvTimeNs = ev.recvTimeNs;
            out->device = k.number;
            out->deviceEvent.kind = DeviceEvent::ModelChanged;
        }
    }
    if (out) { out->deviceEvent.model = d->model; out->deviceEvent.deviceKind = d->kind; }
    return out;
}

std::optional<Event> DeviceTable::onStatus(const Event& ev) {
    uint16_t len = 0;
    if (ev.type == EventType::PlayerStatus) len = ev.status.packetLength;
    else if (ev.type == EventType::MixerStatus) len = 0x38;
    else return std::nullopt;
    Device* d = findMut(ev.device);
    if (!d) return std::nullopt;
    d->lastStatusNs = ev.recvTimeNs;
    if (d->statusLength == len) return std::nullopt;
    d->statusLength = len;
    const Model m = inferModel(d->name, d->statusLength, d->kind);
    if (m == d->model) return std::nullopt;
    d->model = m;
    Event out;
    out.type = EventType::Device;
    out.recvTimeNs = ev.recvTimeNs;
    out.device = d->number;
    out.deviceEvent.kind = DeviceEvent::ModelChanged;
    out.deviceEvent.model = m;
    out.deviceEvent.deviceKind = d->kind;
    return out;
}

void DeviceTable::onBeat(uint8_t number, int64_t nowNs) {
    if (Device* d = findMut(number)) d->lastBeatNs = nowNs;
}

std::vector<Event> DeviceTable::expire(int64_t nowNs) {
    std::vector<Event> gone;
    for (auto it = devices_.begin(); it != devices_.end();) {
        if (nowNs - it->lastKeepAliveNs > kExpiryNs) {
            Event e;
            e.type = EventType::Device;
            e.recvTimeNs = nowNs;
            e.device = it->number;
            e.deviceEvent.kind = DeviceEvent::Left;
            e.deviceEvent.model = it->model;
            e.deviceEvent.deviceKind = it->kind;
            gone.push_back(e);
            it = devices_.erase(it);
        } else {
            ++it;
        }
    }
    return gone;
}

} // namespace shunt::net
