// Control registry (FS-00 section 3). Original Shunt code.
#include "shunt/app/ControlRegistry.h"
#include "shunt/app/Feature.h"
#include <cmath>
#include <cstdio>

namespace shunt::app {

const char* controlKindName(ControlAction::Kind k) {
    switch (k) {
    case ControlAction::Trigger: return "trigger";
    case ControlAction::Toggle: return "toggle";
    case ControlAction::Value: return "value";
    }
    return "trigger";
}

void ControlRegistry::add(ControlAction a) {
    if (a.id.empty() || !a.fn) return;
    std::lock_guard<std::mutex> lk(mu_);
    std::string id = a.id;
    actions_[id] = std::move(a);
}

void ControlRegistry::remove(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    actions_.erase(id);
}

bool ControlRegistry::has(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    return actions_.count(id) != 0;
}

bool ControlRegistry::invoke(const std::string& id, double value) {
    if (coreLockHeldByThisThread()) {
        std::fprintf(stderr, "controls: invoke(%s) refused inside a Core-locked callback (FS-00 locking rule 2)\n", id.c_str());
        return false;
    }
    std::function<void(double)> fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = actions_.find(id);
        if (it == actions_.end()) return false;
        fn = it->second.fn;
    }
    if (!(value >= 0)) value = 0;      // also catches NaN
    if (value > 1) value = 1;
    fn(value);
    return true;
}

std::vector<ControlAction> ControlRegistry::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ControlAction> v;
    v.reserve(actions_.size());
    for (auto& kv : actions_) v.push_back(kv.second);
    return v;
}

} // namespace shunt::app
