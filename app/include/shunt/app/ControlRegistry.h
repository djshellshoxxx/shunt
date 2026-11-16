// Control registry (FS-00 section 3): named actions that the web UI, OSC input and
// (later) MIDI learn can invoke. Original Shunt code.
#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace shunt::app {

struct ControlAction {
    enum Kind { Trigger, Toggle, Value };
    std::string id;                        // "baroffset.cycle", "nudge.plus", "profile.apply:<slug>"
    std::string label;
    Kind kind = Trigger;                   // Value: 0..1 normalised
    std::function<void(double value)> fn;  // must be thread-safe; called on the caller's thread
    std::string group;                     // UI grouping
};

const char* controlKindName(ControlAction::Kind k);   // "trigger" | "toggle" | "value"

// Thread-safe. The registry mutex is a leaf lock: action functions are called after it is
// released, so an action may call public Core methods. invoke() must not be called while the
// Core lock is held (from onEvent/onTick/status); it then refuses and returns false.
class ControlRegistry {
public:
    void add(ControlAction a);                       // replaces an action with the same id; empty id or fn ignored
    void remove(const std::string& id);
    bool invoke(const std::string& id, double value); // value clamped to 0..1 (NaN -> 0); false if unknown
    bool has(const std::string& id) const;
    std::vector<ControlAction> list() const;          // copies, sorted by id

private:
    mutable std::mutex mu_;
    std::map<std::string, ControlAction> actions_;
};

} // namespace shunt::app
