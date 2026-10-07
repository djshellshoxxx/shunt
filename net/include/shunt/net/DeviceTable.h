// Device table with model inference and capability flags (ES-01 section 4, ES-02 section 5).
#pragma once
#include "shunt/net/Events.h"
#include <string>
#include <vector>
#include <optional>

namespace shunt::net {

struct Device {
    uint8_t number = 0;
    DeviceKind kind = DeviceKind::Unknown;
    std::string name;
    std::array<uint8_t, 6> mac{};
    uint32_t ip = 0;
    uint8_t peerCount = 0;
    bool cdj3000Compatible = false;
    Model model = Model::Unknown;
    uint16_t statusLength = 0;
    int64_t lastKeepAliveNs = 0, lastStatusNs = 0, lastBeatNs = 0;
};

Model inferModel(const std::string& name, uint16_t statusLength, DeviceKind kind);
Capabilities capabilitiesFor(Model m);
const char* modelName(Model m);

class DeviceTable {
public:
    static constexpr int64_t kExpiryNs = 10'000'000'000LL;

    // Returns a Joined / ModelChanged event when the table changed.
    std::optional<Event> onKeepAlive(const Event& keepAlive);
    std::optional<Event> onStatus(const Event& status);   // updates lastStatus and model from length
    void onBeat(uint8_t number, int64_t nowNs);
    // Removes devices unseen for kExpiryNs; returns Left events.
    std::vector<Event> expire(int64_t nowNs);

    const Device* find(uint8_t number) const;
    const std::vector<Device>& devices() const { return devices_; }
    bool isTaken(uint8_t number) const { return find(number) != nullptr; }
    bool mixerPresent() const;
    size_t size() const { return devices_.size(); }
    void clear() { devices_.clear(); }

private:
    Device* findMut(uint8_t number);
    std::vector<Device> devices_;
};

} // namespace shunt::net
