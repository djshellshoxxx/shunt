// Feature registry singleton and Core lock marker (FS-00 section 1). Original Shunt code.
#include "shunt/app/Feature.h"
#include <algorithm>

namespace shunt::app {

namespace {
thread_local bool t_coreLocked = false;
}

FeatureRegistry& FeatureRegistry::instance() {
    static FeatureRegistry r;
    return r;
}

void FeatureRegistry::add(FeatureFactory f) {
    if (!f) return;
    std::lock_guard<std::mutex> lk(mu_);
    factories_.push_back(std::move(f));
}

std::vector<std::unique_ptr<IFeature>> FeatureRegistry::createAll() const {
    std::vector<FeatureFactory> fs;
    { std::lock_guard<std::mutex> lk(mu_); fs = factories_; }
    std::vector<std::unique_ptr<IFeature>> out;
    for (auto& f : fs) if (auto p = f()) out.push_back(std::move(p));
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a->id() < b->id(); });
    return out;
}

size_t FeatureRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return factories_.size();
}

bool validFeatureId(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

bool coreLockHeldByThisThread() { return t_coreLocked; }
CoreLockedScope::CoreLockedScope() : prev_(t_coreLocked) { t_coreLocked = true; }
CoreLockedScope::~CoreLockedScope() { t_coreLocked = prev_; }

} // namespace shunt::app
