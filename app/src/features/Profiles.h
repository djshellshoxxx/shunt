// Venue profiles (FS-07, docs/specs/FS-07-venue-profiles.md): named snapshots of a whitelisted
// subset of the settings, stored one file per profile in <config dir>/profiles/<slug>.json.
// The pure part (slugs, whitelist snapshot, validation, diff, patch) has no I/O and is unit tested;
// ProfilesFeature wires it to FS-00 (routes /api/f/profiles/*, actions profile.next / profile.prev /
// profile.apply:<slug>, status.features.profiles). Settings reach the feature through ProfileBackend
// (full config read, Core::applyConfig semantics, config dir) because FeatureHost has no such
// call yet (see the spec's Deviations section). Original Shunt code.
#pragma once
#include "shunt/app/Feature.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shunt::app::profiles {

constexpr int kSchema = 1;
constexpr size_t kMaxProfiles = 100;
constexpr size_t kMaxFileBytes = 64 * 1024;
constexpr size_t kMaxName = 80;

// ---- pure logic --------------------------------------------------------------------------------

// Lower-case [a-z0-9-], every other byte (spaces, punctuation, non-ASCII, NUL, '/', '.') becomes '-',
// runs collapse, ends trimmed, max 40 chars, "profile" when nothing is left.
std::string slugify(const std::string& name);
bool validSlug(const std::string& s);                       // [a-z0-9-]{1,40}
// base, base-2, base-3 ... (base is shortened so the suffix fits 40 chars) until !taken(candidate).
std::string uniqueSlug(const std::string& base, const std::function<bool(const std::string&)>& taken);
bool validName(const std::string& name);                    // 1..80 bytes, trimmed, no control characters

struct Profile {
    std::string name, slug, created;
    bool includeInterface = false;
    Json settings = Json::object();
};

struct Change {
    std::vector<std::string> path;
    Json current, profile;
    std::string pathText() const;                           // "midi.port"
};

// Whitelisted subset of a full config (Settings::toJson): mode, deviceNumber, beatOnly, profile,
// latencyMs, link, midi, osc, venue, performer, interface, features.<id> (not "profiles").
Json snapshot(const Json& config);
// Validates and normalises a settings object: unknown keys are dropped (warnings), wrong types are
// errors naming the JSON path, numbers are clamped like Settings::fromJson (warning).
bool validateSettings(const Json& in, Json& out, std::vector<std::string>& warnings, std::string& error);
// Parses a profile document (file or import): size, schema 1, name, settings. `slug` is filled only
// when the document carries a valid one.
bool parseProfile(const std::string& text, Profile& out, std::vector<std::string>& warnings, std::string& error);
std::string serialize(const Profile& p);

bool jsonEqual(const Json& a, const Json& b);
// Changes Apply would make on `currentConfig`. `interface` is compared only when includeInterface.
std::vector<Change> diff(const Json& currentConfig, const Profile& p);
bool needsRestart(const std::vector<Change>& c);            // interface, mode or deviceNumber differ
Json patchFrom(const std::vector<Change>& c);               // minimal POST /api/config patch

// ---- feature -----------------------------------------------------------------------------------

// How the feature reaches the rest of the settings. Core implements it once FeatureHost grows the
// calls; tests use a fake. Called without any feature lock held.
struct ProfileBackend {
    virtual ~ProfileBackend() = default;
    virtual Json config() = 0;                                          // Settings::toJson() (Core::config)
    virtual bool applyConfig(const Json& patch, std::string& error) = 0; // POST /api/config semantics
    virtual std::string configDir() = 0;                                 // directory of config.json, "" if unknown
};

struct ProfilesOptions {
    std::shared_ptr<ProfileBackend> backend;     // null: features-only fallback built on FeatureHost
    std::string dir;                             // profiles directory; empty: <config dir>/profiles
    int pollMs = 2000;                           // background refresh of `dirty`; 0 = no thread (tests)
    std::function<bool()> beforeRename;          // test hook: return false to abort a save between temp file and rename
};

} // namespace shunt::app::profiles

namespace shunt::app {

class ProfilesFeature : public IFeature {
public:
    ProfilesFeature();
    explicit ProfilesFeature(profiles::ProfilesOptions o);
    ~ProfilesFeature() override;
    std::string id() const override { return "profiles"; }
    void init(FeatureHost& host, const Json& settings) override;
    void applySettings(const Json& settings) override;
    Json status() const override;
    std::optional<HttpResponse> handle(const HttpRequest& req, const std::string& subpath) override;
    void shutdown() override;
    void refreshNow();                           // recompute active/dirty/actions now (tests, handle paths)

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace shunt::app
