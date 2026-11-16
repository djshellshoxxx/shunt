// Venue profiles (FS-07): see Profiles.h. Files: <config dir>/profiles/<slug>.json, written atomically
// (temp file, fsync, rename); only validated slugs are ever turned into paths. Locking: `mu_` guards the
// cached state read by status() (Core lock held, so it is a leaf and never held across a call), `opMu_`
// serialises file operations and is never held while calling the host or the backend (FS-00 rule 3).
// All file I/O happens in handle(), in the feature's own refresh thread, or in init/applySettings (no Core
// lock). Original Shunt code.
#include "Profiles.h"
#include "shunt/app/Settings.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace shunt::app::profiles {

// ---- slugs and names ---------------------------------------------------------------------------

std::string slugify(const std::string& name) {
    std::string s;
    for (unsigned char c : name) {
        char o = '-';
        if (c >= 'a' && c <= 'z') o = char(c);
        else if (c >= 'A' && c <= 'Z') o = char(c - 'A' + 'a');
        else if (c >= '0' && c <= '9') o = char(c);
        if (o == '-' && !s.empty() && s.back() == '-') continue;
        s.push_back(o);
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    while (!s.empty() && s.front() == '-') s.erase(s.begin());
    if (s.size() > 40) s.resize(40);
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s.empty() ? "profile" : s;
}

bool validSlug(const std::string& s) {
    if (s.empty() || s.size() > 40) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

std::string uniqueSlug(const std::string& base, const std::function<bool(const std::string&)>& taken) {
    std::string b = validSlug(base) ? base : slugify(base);
    if (!taken(b)) return b;
    for (int n = 2; n < 100000; ++n) {
        const std::string suf = "-" + std::to_string(n);
        std::string c = b.substr(0, 40 - suf.size()) + suf;
        if (!taken(c)) return c;
    }
    return b + "-x";
}

bool validName(const std::string& n) {
    if (n.empty() || n.size() > kMaxName) return false;
    if (n.front() == ' ' || n.back() == ' ') return false;
    for (unsigned char c : n)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

std::string Change::pathText() const {
    std::string s;
    for (auto& p : path) { if (!s.empty()) s += '.'; s += p; }
    return s;
}

// ---- whitelist and validation ------------------------------------------------------------------

namespace {
enum Kind { KBool, KStr, KNum, KInt };
struct Spec { const char* key; Kind kind; double lo, hi; };

const Spec kTop[] = {{"mode", KStr, 0, 0}, {"deviceNumber", KInt, 1, 6}, {"beatOnly", KBool, 0, 0}, {"profile", KStr, 0, 0},
                     {"latencyMs", KNum, -20, 50}, {"venue", KStr, 0, 0}, {"performer", KStr, 0, 0}, {"interface", KStr, 0, 0}};
const Spec kLink[] = {{"enabled", KBool, 0, 0}};
const Spec kMidi[] = {{"enabled", KBool, 0, 0}, {"port", KStr, 0, 0}, {"pause", KStr, 0, 0}, {"startOnReset", KBool, 0, 0},
                      {"sppOnReset", KBool, 0, 0}, {"startOnFirstLock", KBool, 0, 0}};
const Spec kOsc[] = {{"enabled", KBool, 0, 0}, {"host", KStr, 0, 0}, {"port", KInt, 1, 65535}, {"profile", KStr, 0, 0},
                     {"qlabMasterCue", KInt, 0, 9999}, {"qlabTrackCue", KInt, 0, 9999}, {"inEnabled", KBool, 0, 0},
                     {"inPort", KInt, 1, 65535}};
constexpr size_t kMaxStr = 256;

const char* typeName(Json::Type t) {
    switch (t) { case Json::Null: return "null"; case Json::Bool: return "boolean"; case Json::Number: return "number";
                 case Json::String: return "string"; case Json::Array: return "array"; default: return "object"; }
}

template <size_t N>
bool pickScalars(const Json& in, const Spec (&specs)[N], const std::string& base, Json& out, std::vector<std::string>& warn, std::string& err) {
    for (auto& kv : in.members()) {
        const Spec* sp = nullptr;
        for (auto& s : specs) if (kv.first == s.key) sp = &s;
        const std::string path = base.empty() ? kv.first : base + "." + kv.first;
        if (!sp) { warn.push_back("dropped unknown key " + path); continue; }
        const Json& v = kv.second;
        switch (sp->kind) {
            case KBool:
                if (v.type() != Json::Bool) { err = path + ": expected boolean, got " + typeName(v.type()); return false; }
                out[kv.first] = v.asBool();
                break;
            case KStr:
                if (v.type() != Json::String) { err = path + ": expected string, got " + typeName(v.type()); return false; }
                if (v.asString().size() > kMaxStr) { err = path + ": string longer than 256 bytes"; return false; }
                out[kv.first] = v.asString();
                break;
            default: {
                if (v.type() != Json::Number) { err = path + ": expected number, got " + typeName(v.type()); return false; }
                double d = v.asNumber();
                if (!std::isfinite(d)) { err = path + ": number is not finite"; return false; }
                if (sp->kind == KInt) d = std::round(d);
                const double c = std::min(sp->hi, std::max(sp->lo, d));
                if (c != d) warn.push_back(path + " clamped to " + std::to_string(c));
                if (sp->kind == KInt) out[kv.first] = int(c);
                else out[kv.first] = c;
            }
        }
    }
    return true;
}

Json stripNulls(const Json& j) {
    if (j.isObject()) {
        Json o = Json::object();
        for (auto& kv : j.members()) if (!kv.second.isNull()) o[kv.first] = stripNulls(kv.second);
        return o;
    }
    if (j.isArray()) {
        Json a = Json::array();
        for (auto& v : j.items()) a.push(stripNulls(v));
        return a;
    }
    return j;
}
} // namespace

Json snapshot(const Json& cfg) {
    Json s = Json::object();
    for (auto& sp : kTop) if (cfg.has(sp.key)) s[sp.key] = cfg.get(sp.key);
    for (const char* sec : {"link", "midi", "osc"}) {
        if (!cfg.get(sec).isObject()) continue;
        Json o = Json::object();
        const Spec* b = std::string(sec) == "link" ? kLink : std::string(sec) == "midi" ? kMidi : kOsc;
        const size_t n = std::string(sec) == "link" ? std::size(kLink) : std::string(sec) == "midi" ? std::size(kMidi) : std::size(kOsc);
        for (size_t i = 0; i < n; ++i) if (cfg.get(sec).has(b[i].key)) o[b[i].key] = cfg.get(sec).get(b[i].key);
        s[sec] = o;
    }
    if (cfg.get("features").isObject()) {
        Json f = Json::object();
        for (auto& kv : cfg.get("features").members())
            if (kv.first != "profiles" && kv.second.isObject() && validFeatureId(kv.first)) f[kv.first] = stripNulls(kv.second);
        s["features"] = f;
    }
    return s;
}

bool validateSettings(const Json& in, Json& out, std::vector<std::string>& warn, std::string& err) {
    if (!in.isObject()) { err = "settings: expected object, got " + std::string(typeName(in.type())); return false; }
    Json o = Json::object();
    Json top = Json::object();
    for (auto& kv : in.members()) {   // scalars first via a filtered view
        bool known = false;
        for (auto& s : kTop) if (kv.first == s.key) known = true;
        if (known) top[kv.first] = kv.second;
    }
    if (!pickScalars(top, kTop, "settings", o, warn, err)) return false;
    if (o.has("mode") && o.get("mode").asString() != "follow" && o.get("mode").asString() != "passive") {
        warn.push_back("dropped settings.mode: must be follow or passive");
        o.erase("mode");
    }
    if (o.has("profile") && o.get("profile").asString() != "stage" && o.get("profile").asString() != "rehearsal") {
        warn.push_back("dropped settings.profile: must be stage or rehearsal");
        o.erase("profile");
    }
    for (auto& kv : in.members()) {
        const std::string& k = kv.first;
        const std::string path = "settings." + k;
        bool scalar = false;
        for (auto& s : kTop) if (k == s.key) scalar = true;
        if (scalar) continue;
        if (k == "link" || k == "midi" || k == "osc") {
            if (!kv.second.isObject()) { err = path + ": expected object, got " + typeName(kv.second.type()); return false; }
            Json sec = Json::object();
            bool ok = k == "link" ? pickScalars(kv.second, kLink, path, sec, warn, err)
                    : k == "midi" ? pickScalars(kv.second, kMidi, path, sec, warn, err)
                                  : pickScalars(kv.second, kOsc, path, sec, warn, err);
            if (!ok) return false;
            if (k == "midi" && sec.has("pause") && sec.get("pause").asString() != "keep" && sec.get("pause").asString() != "stop") {
                warn.push_back("dropped settings.midi.pause: must be keep or stop");
                sec.erase("pause");
            }
            o[k] = sec;
        } else if (k == "features") {
            if (!kv.second.isObject()) { err = path + ": expected object, got " + typeName(kv.second.type()); return false; }
            Json f = Json::object();
            for (auto& fk : kv.second.members()) {
                const std::string fp = path + "." + fk.first;
                if (!validFeatureId(fk.first)) { warn.push_back("dropped " + fp + ": invalid feature id"); continue; }
                if (fk.first == "profiles") { warn.push_back("dropped " + fp + ": not part of a profile"); continue; }
                if (!fk.second.isObject()) { err = fp + ": expected object, got " + typeName(fk.second.type()); return false; }
                f[fk.first] = stripNulls(fk.second);
            }
            o["features"] = f;
        } else {
            warn.push_back("dropped unknown key " + path);
        }
    }
    out = o;
    return true;
}

// ---- documents ---------------------------------------------------------------------------------

bool parseProfile(const std::string& text, Profile& out, std::vector<std::string>& warn, std::string& err) {
    if (text.size() > kMaxFileBytes) { err = "profile larger than 64 KB"; return false; }
    Json j;
    std::string perr;
    if (!Json::parse(text, j, &perr)) { err = "not valid JSON: " + perr; return false; }
    if (!j.isObject()) { err = "expected a JSON object"; return false; }
    const Json& sc = j.get("schema");
    if (sc.type() != Json::Number || sc.asNumber() != kSchema) { err = "schema: only schema 1 is supported"; return false; }
    const Json& nm = j.get("name");
    if (nm.type() != Json::String) { err = "name: expected string"; return false; }
    if (!validName(nm.asString())) { err = "name: 1 to 80 characters, no control characters, no leading or trailing space"; return false; }
    Profile p;
    p.name = nm.asString();
    if (j.has("slug")) {
        if (j.get("slug").type() != Json::String) { err = "slug: expected string"; return false; }
        if (validSlug(j.get("slug").asString())) p.slug = j.get("slug").asString();
        else warn.push_back("ignored invalid slug");
    }
    if (j.has("created")) {
        if (j.get("created").type() != Json::String) { err = "created: expected string"; return false; }
        if (j.get("created").asString().size() <= 40) p.created = j.get("created").asString();
    }
    if (j.has("includeInterface")) {
        if (j.get("includeInterface").type() != Json::Bool) { err = "includeInterface: expected boolean"; return false; }
        p.includeInterface = j.get("includeInterface").asBool();
    }
    if (!j.has("settings")) { err = "settings: missing"; return false; }
    if (!validateSettings(j.get("settings"), p.settings, warn, err)) return false;
    for (auto& kv : j.members())
        if (kv.first != "schema" && kv.first != "name" && kv.first != "slug" && kv.first != "created" && kv.first != "includeInterface" && kv.first != "settings")
            warn.push_back("dropped unknown key " + kv.first);
    out = std::move(p);
    return true;
}

std::string serialize(const Profile& p) {
    Json j = Json::object();
    j["schema"] = kSchema;
    j["name"] = p.name;
    j["slug"] = p.slug;
    j["created"] = p.created;
    j["includeInterface"] = p.includeInterface;
    j["settings"] = p.settings;
    return j.dump();
}

// ---- diff --------------------------------------------------------------------------------------

bool jsonEqual(const Json& a, const Json& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case Json::Null: return true;
        case Json::Bool: return a.asBool() == b.asBool();
        case Json::Number: return std::fabs(a.asNumber() - b.asNumber()) < 1e-9;
        case Json::String: return a.asString() == b.asString();
        case Json::Array:
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.items().size(); ++i) if (!jsonEqual(a.items()[i], b.items()[i])) return false;
            return true;
        default:
            if (a.size() != b.size()) return false;
            for (auto& kv : a.members()) if (!b.has(kv.first) || !jsonEqual(kv.second, b.get(kv.first))) return false;
            return true;
    }
}

namespace {
constexpr size_t kMaxChanges = 2000;
void walk(std::vector<std::string>& path, const Json& cur, const Json& prof, bool withInterface, std::vector<Change>& out) {
    if (out.size() >= kMaxChanges) return;
    if (prof.isObject()) {
        for (auto& kv : prof.members()) {
            if (path.empty() && kv.first == "interface" && !withInterface) continue;
            path.push_back(kv.first);
            walk(path, cur.get(kv.first), kv.second, withInterface, out);
            path.pop_back();
        }
        return;
    }
    if (!jsonEqual(cur, prof)) out.push_back({path, cur, prof});
}
} // namespace

std::vector<Change> diff(const Json& cfg, const Profile& p) {
    std::vector<Change> out;
    std::vector<std::string> path;
    walk(path, cfg, p.settings, p.includeInterface, out);
    return out;
}

bool needsRestart(const std::vector<Change>& c) {
    for (auto& ch : c)
        if (ch.path.size() == 1 && (ch.path[0] == "interface" || ch.path[0] == "mode" || ch.path[0] == "deviceNumber")) return true;
    return false;
}

Json patchFrom(const std::vector<Change>& c) {
    Json patch = Json::object();
    for (auto& ch : c) {
        Json* n = &patch;
        for (auto& k : ch.path) n = &(*n)[k];
        *n = ch.profile;
    }
    return patch;
}

} // namespace shunt::app::profiles

// ---- feature -----------------------------------------------------------------------------------

namespace shunt::app {
using namespace profiles;

namespace {
HttpResponse reply(const Json& j, int status = 200) {
    HttpResponse r;
    r.status = status;
    r.body = j.dump();
    return r;
}
HttpResponse err(int status, const std::string& msg) {
    Json j = Json::object();
    j["ok"] = false;
    j["error"] = msg;
    return reply(j, status);
}
Json strArray(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (auto& s : v) a.push(s);
    return a;
}
std::string isoUtc(int64_t ms) {
    const time_t t = time_t(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}
bool mkdirs(const std::string& dir) {
    for (size_t i = 1; i < dir.size(); ++i)
        if (dir[i] == '/') ::mkdir(dir.substr(0, i).c_str(), 0755);
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    struct stat st;
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// Features-only fallback used when no ProfileBackend is injected: FeatureHost can read and save
// settings.features.<id> but not the rest of the settings (reported seam).
// Real Core settings through FeatureHost's whole-config seam.
class CoreHostBackend : public ProfileBackend {
public:
    explicit CoreHostBackend(FeatureHost& h) : host_(h) {}
    Json config() override { return host_.config(); }
    bool applyConfig(const Json& patch, std::string& error) override { return host_.applyConfig(patch, error); }
    std::string configDir() override { return host_.configDir(); }
private:
    FeatureHost& host_;
};

// Fallback for hosts without whole-config access: only features.* is visible.
class HostBackend : public ProfileBackend {
public:
    explicit HostBackend(FeatureHost& h) : host_(h) {
        for (auto& f : FeatureRegistry::instance().createAll()) {
            try { ids_.push_back(f->id()); } catch (...) {}
        }
    }
    Json config() override {
        Json c = Json::object();
        Json& f = c["features"];
        f = Json::object();
        for (auto& id : ids_) if (validFeatureId(id)) f[id] = host_.settings(id);
        return c;
    }
    bool applyConfig(const Json& patch, std::string& error) override {
        for (auto& kv : patch.members())
            if (kv.first != "features") { error = "core settings are not reachable from a feature yet (FeatureHost seam missing)"; return false; }
        for (auto& kv : patch.get("features").members()) {
            Json cur = host_.settings(kv.first);
            cur.merge(kv.second);
            if (!host_.saveSettings(kv.first, cur)) { error = "could not save settings for " + kv.first; return false; }
        }
        return true;
    }
    std::string configDir() override { return ""; }
private:
    FeatureHost& host_;
    std::vector<std::string> ids_;
};
} // namespace

struct ProfilesFeature::Impl : std::enable_shared_from_this<Impl> {
    ProfilesOptions opts;
    FeatureHost* host = nullptr;
    std::shared_ptr<ProfileBackend> backend;
    bool realBackend = false;

    std::mutex opMu;     // file operations
    std::mutex mu;       // cached state (leaf)
    bool enabled = false;
    std::string active, activeName, warning;
    bool dirty = false;
    int count = 0;
    int64_t rev = 0;
    std::set<std::string> registered;
    std::vector<std::string> order;     // slugs, alphabetical by name (for next/prev)
    std::string dirCache;

    std::mutex tmu;
    std::condition_variable tcv;
    bool stop = false;
    std::thread th;

    // ---- paths and files ----
    std::string dir() {
        if (!opts.dir.empty()) return opts.dir;
        if (backend) {
            const std::string d = backend->configDir();
            if (!d.empty()) return d + "/profiles";
        }
        std::string p = Settings::defaultPath();
        const size_t s = p.rfind('/');
        return (s == std::string::npos ? std::string(".") : p.substr(0, s)) + "/profiles";
    }
    static std::string pathOf(const std::string& d, const std::string& slug) { return d + "/" + slug + ".json"; }

    static bool readFile(const std::string& path, std::string& out, std::string& e) {
        int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
        if (fd < 0) { e = errno == ENOENT ? "not found" : "cannot open"; return false; }
        std::string s;
        char buf[4096];
        for (;;) {
            ssize_t n = ::read(fd, buf, sizeof buf);
            if (n < 0) { ::close(fd); e = "read error"; return false; }
            if (n == 0) break;
            s.append(buf, size_t(n));
            if (s.size() > kMaxFileBytes) { ::close(fd); e = "file larger than 64 KB"; return false; }
        }
        ::close(fd);
        out = std::move(s);
        return true;
    }

    // Caller holds opMu.
    bool loadLocked(const std::string& slug, Profile& p, std::vector<std::string>& warn, std::string& e) {
        if (!validSlug(slug)) { e = "invalid slug"; return false; }
        std::string text;
        if (!readFile(pathOf(dir(), slug), text, e)) return false;
        if (!parseProfile(text, p, warn, e)) return false;
        p.slug = slug;   // the file name wins over the stored slug
        return true;
    }

    std::vector<Profile> scanLocked() {
        std::vector<Profile> v;
        const std::string d = dir();
        if (DIR* dp = ::opendir(d.c_str())) {
            std::vector<std::string> slugs;
            while (dirent* de = ::readdir(dp)) {
                const std::string n = de->d_name;
                if (n.size() < 6 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
                const std::string slug = n.substr(0, n.size() - 5);
                if (validSlug(slug)) slugs.push_back(slug);
            }
            ::closedir(dp);
            std::sort(slugs.begin(), slugs.end());
            for (auto& s : slugs) {
                if (v.size() >= kMaxProfiles) break;
                Profile p;
                std::vector<std::string> w;
                std::string e;
                if (loadLocked(s, p, w, e)) v.push_back(std::move(p));
            }
        }
        std::sort(v.begin(), v.end(), [](const Profile& a, const Profile& b) {
            const std::string la = lower(a.name), lb = lower(b.name);
            return la != lb ? la < lb : a.slug < b.slug;
        });
        return v;
    }

    // Caller holds opMu. Atomic: temp file in the same directory, fsync, rename.
    bool writeLocked(const Profile& p, std::string& e) {
        const std::string d = dir();
        if (!mkdirs(d)) { e = "cannot create the profiles folder"; return false; }
        const std::string body = serialize(p);
        if (body.size() > kMaxFileBytes) { e = "profile larger than 64 KB"; return false; }
        const std::string fin = pathOf(d, p.slug), tmp = d + "/." + p.slug + ".json.tmp";
        ::unlink(tmp.c_str());
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
        if (fd < 0) { e = "cannot write the profile"; return false; }
        size_t off = 0;
        while (off < body.size()) {
            ssize_t n = ::write(fd, body.data() + off, body.size() - off);
            if (n <= 0) { ::close(fd); ::unlink(tmp.c_str()); e = "write error"; return false; }
            off += size_t(n);
        }
        ::fsync(fd);
        ::close(fd);
        if (opts.beforeRename && !opts.beforeRename()) { e = "aborted before rename"; return false; }   // test hook: leaves the temp file
        if (::rename(tmp.c_str(), fin.c_str()) != 0) { ::unlink(tmp.c_str()); e = "rename failed"; return false; }
        return true;
    }

    // ---- cached state and actions ----
    void setWarning(const std::string& w) {
        std::lock_guard<std::mutex> lk(mu);
        warning = w;
    }

    std::string activeFromHost() {
        if (!host) return "";
        const Json s = host->settings("profiles");
        const std::string a = s.get("active").asString("");
        return validSlug(a) ? a : "";
    }

    // Recompute active/dirty (one config read, one file read). No Core lock may be held.
    void refreshDirty() {
        if (!host || !backend) return;
        const std::string act = activeFromHost();
        bool d = false;
        std::string name;
        if (!act.empty()) {
            Profile p;
            std::vector<std::string> w;
            std::string e;
            bool ok;
            { std::lock_guard<std::mutex> lk(opMu); ok = loadLocked(act, p, w, e); }
            if (ok) {
                name = p.name;
                d = !diff(backend->config(), p).empty();
            }
        }
        std::lock_guard<std::mutex> lk(mu);
        if (active != act || dirty != d || activeName != name) ++rev;
        active = act;
        activeName = name;
        dirty = d;
    }

    // Rescan the folder and re-register actions. No Core lock may be held.
    void reindex() {
        if (!host) return;
        std::vector<Profile> list;
        { std::lock_guard<std::mutex> lk(opMu); list = scanLocked(); }
        bool on;
        { std::lock_guard<std::mutex> lk(mu); on = enabled; }
        std::set<std::string> want;
        std::vector<std::string> ord;
        if (on) {
            want.insert("profile.next");
            want.insert("profile.prev");
        }
        for (auto& p : list) {
            ord.push_back(p.slug);
            if (on) want.insert("profile.apply:" + p.slug);
        }
        std::set<std::string> old;
        { std::lock_guard<std::mutex> lk(mu); old = registered; }
        for (auto& id : old) if (!want.count(id)) host->controls().remove(id);
        std::weak_ptr<Impl> self = weak_from_this();
        auto call = [self](std::function<void(Impl&)> f) { if (auto m = self.lock()) f(*m); };
        for (auto& p : list) {
            const std::string id = "profile.apply:" + p.slug;
            if (!on) break;
            ControlAction a;
            a.id = id; a.label = "Profile: " + p.name; a.kind = ControlAction::Trigger; a.group = "Profiles";
            a.fn = [call, slug = p.slug](double) { call([slug](Impl& m) { m.footswitchApply(slug); }); };
            host->controls().add(std::move(a));       // replaces same id: label follows renames
        }
        if (on) {
            ControlAction n, pv;
            n.id = "profile.next"; n.label = "Profile: next"; n.kind = ControlAction::Trigger; n.group = "Profiles";
            n.fn = [call](double) { call([](Impl& m) { m.footswitchStep(+1); }); };
            pv.id = "profile.prev"; pv.label = "Profile: previous"; pv.kind = ControlAction::Trigger; pv.group = "Profiles";
            pv.fn = [call](double) { call([](Impl& m) { m.footswitchStep(-1); }); };
            host->controls().add(std::move(n));
            host->controls().add(std::move(pv));
        }
        std::lock_guard<std::mutex> lk(mu);
        if (registered != want || order != ord || count != int(list.size())) ++rev;
        registered = want;
        order = ord;
        count = int(list.size());
    }

    void refresh() { reindex(); refreshDirty(); }

    // ---- apply ----
    struct Applied { int status = 200; Json body = Json::object(); };

    Applied apply(const std::string& slug, bool confirm, bool footswitch) {
        Applied r;
        Profile p;
        std::vector<std::string> warn;
        std::string e;
        bool ok;
        { std::lock_guard<std::mutex> lk(opMu); ok = loadLocked(slug, p, warn, e); }
        if (!ok) {
            r.status = e == "not found" ? 404 : e == "invalid slug" ? 400 : 422;
            r.body["ok"] = false;
            r.body["error"] = e;
            return r;
        }
        const std::vector<Change> ch = diff(backend->config(), p);
        const bool restart = needsRestart(ch);
        if (restart && (!confirm || footswitch)) {
            r.status = 409;
            r.body = diffJson(p, ch);
            r.body["ok"] = false;
            r.body["error"] = footswitch ? "this profile changes interface, mode or device number; apply it from the Profiles page"
                                         : "this profile restarts the network; confirm to apply";
            return r;
        }
        Json patch = patchFrom(ch);
        patch["features"]["profiles"]["active"] = slug;
        std::string perr;
        if (!backend->applyConfig(patch, perr)) {
            r.status = 500;
            r.body["ok"] = false;
            r.body["error"] = perr.empty() ? "could not apply the settings" : perr;
            return r;
        }
        setWarning("");
        refresh();
        r.body["ok"] = true;
        r.body["slug"] = slug;
        r.body["applied"] = int(ch.size());
        r.body["restart"] = restart;
        r.body["warnings"] = strArray(warn);
        return r;
    }

    void footswitchApply(const std::string& slug) {
        Applied a;
        try { a = apply(slug, false, true); } catch (...) { return; }
        if (a.status == 200) return;
        setWarning("profile " + slug + ": " + a.body.get("error").asString("not applied"));
    }

    void footswitchStep(int dir) {
        std::vector<std::string> ord;
        std::string act;
        { std::lock_guard<std::mutex> lk(mu); ord = order; act = active; }
        if (ord.empty()) return;
        size_t i = 0;
        auto it = std::find(ord.begin(), ord.end(), act);
        if (it == ord.end()) i = dir > 0 ? 0 : ord.size() - 1;
        else i = size_t((int(it - ord.begin()) + dir + int(ord.size())) % int(ord.size()));
        footswitchApply(ord[i]);
    }

    Json diffJson(const Profile& p, const std::vector<Change>& ch) {
        Json j = Json::object();
        j["slug"] = p.slug;
        Json a = Json::array();
        for (auto& c : ch) {
            Json o = Json::object();
            o["path"] = c.pathText();
            o["current"] = c.current;
            o["profile"] = c.profile;
            a.push(o);
        }
        j["changes"] = a;
        const bool restart = needsRestart(ch);
        j["restart"] = restart;
        j["needsConfirm"] = restart;
        j["includeInterface"] = p.includeInterface;
        return j;
    }

    Json listJson(const std::vector<Profile>& list) {
        Json j = Json::object();
        Json a = Json::array();
        for (auto& p : list) {
            Json o = Json::object();
            o["slug"] = p.slug; o["name"] = p.name; o["created"] = p.created; o["includeInterface"] = p.includeInterface;
            a.push(o);
        }
        j["profiles"] = a;
        std::lock_guard<std::mutex> lk(mu);
        j["active"] = active;
        j["activeName"] = activeName;
        j["dirty"] = dirty;
        j["max"] = int(kMaxProfiles);
        j["backend"] = realBackend ? "core" : "features-only";
        return j;
    }

    // ---- HTTP ----
    std::optional<HttpResponse> handle(const HttpRequest& req, const std::string& sub) {
        if (!host || !backend) return err(503, "profiles not initialised");
        const bool get = req.method == "GET" || req.method == "HEAD";
        const bool post = req.method == "POST";
        if (sub == "list") {
            if (!get) return err(405, "GET only");
            std::vector<Profile> list;
            { std::lock_guard<std::mutex> lk(opMu); list = scanLocked(); }
            refresh();
            return reply(listJson(list));
        }
        if (sub == "export") {
            if (!get) return err(405, "GET only");
            const std::string slug = req.queryParam("slug");
            if (!validSlug(slug)) return err(400, "invalid slug");
            std::string text, e;
            { std::lock_guard<std::mutex> lk(opMu); if (!readFile(pathOf(dir(), slug), text, e)) return err(e == "not found" ? 404 : 422, e); }
            HttpResponse r;
            r.body = text;
            r.headers.emplace_back("Content-Disposition", "attachment; filename=\"" + slug + ".json\"");
            return r;
        }
        static const std::set<std::string> posts = {"save", "diff", "apply", "rename", "duplicate", "delete", "import"};
        if (!posts.count(sub)) return std::nullopt;
        if (!post) return err(405, "POST only");
        if (req.body.size() > kMaxFileBytes) return err(413, "body larger than 64 KB");
        Json b;
        std::string pe;
        if (!Json::parse(req.body.empty() ? "{}" : req.body, b, &pe) || !b.isObject()) return err(400, "expected a JSON object body");
        if (sub == "import") return doImport(req.body);
        if (sub == "save") return doSave(b);
        const Json& sj = b.get("slug");
        if (sj.type() != Json::String || !validSlug(sj.asString())) return err(400, "slug: expected a valid slug");
        const std::string slug = sj.asString();
        if (sub == "apply") {
            if (b.has("confirm") && b.get("confirm").type() != Json::Bool) return err(400, "confirm: expected boolean");
            Applied a = apply(slug, b.get("confirm").asBool(), false);
            return reply(a.body, a.status);
        }
        if (sub == "diff") {
            Profile p;
            std::vector<std::string> w;
            std::string e;
            bool ok;
            { std::lock_guard<std::mutex> lk(opMu); ok = loadLocked(slug, p, w, e); }
            if (!ok) return err(e == "not found" ? 404 : 422, e);
            Json j = diffJson(p, diff(backend->config(), p));
            j["ok"] = true;
            j["warnings"] = strArray(w);
            return reply(j);
        }
        if (sub == "delete") return doDelete(slug);
        if (sub == "rename") return doRename(slug, b);
        return doDuplicate(slug, b);
    }

    HttpResponse doSave(const Json& b) {
        const Json& nm = b.get("name");
        if (nm.type() != Json::String || !validName(nm.asString())) return err(400, "name: 1 to 80 characters, no control characters, no leading or trailing space");
        if (b.has("includeInterface") && b.get("includeInterface").type() != Json::Bool) return err(400, "includeInterface: expected boolean");
        if (b.has("overwrite") && b.get("overwrite").type() != Json::Bool) return err(400, "overwrite: expected boolean");
        if (b.has("slug") && (b.get("slug").type() != Json::String || !validSlug(b.get("slug").asString()))) return err(400, "slug: expected a valid slug");
        const bool overwrite = b.get("overwrite").asBool();
        Profile p;
        p.name = nm.asString();
        p.includeInterface = b.get("includeInterface").asBool();
        p.settings = snapshot(backend->config());
        std::string e;
        {
            std::lock_guard<std::mutex> lk(opMu);
            const std::vector<Profile> list = scanLocked();
            auto find = [&](const std::string& s) -> const Profile* { for (auto& x : list) if (x.slug == s) return &x; return nullptr; };
            std::string slug;
            const Profile* existing = nullptr;
            if (b.has("slug")) {
                slug = b.get("slug").asString();
                existing = find(slug);
            } else {
                slug = slugify(p.name);
                existing = find(slug);
                if (!existing || existing->name != p.name) {      // a profile with exactly this name wins, else -2, -3 ...
                    existing = nullptr;
                    for (auto& x : list) if (x.name == p.name) { existing = &x; slug = x.slug; break; }
                    if (!existing) slug = uniqueSlug(slug, [&](const std::string& s2) { return find(s2) != nullptr; });
                }
            }
            if (existing && !overwrite) {
                Json j = Json::object();
                j["ok"] = false; j["error"] = "a profile with this name exists; save with overwrite to replace it"; j["exists"] = true; j["slug"] = slug;
                return reply(j, 409);
            }
            if (!existing && list.size() >= kMaxProfiles) return err(409, "at most 100 profiles");
            p.slug = slug;
            p.created = existing && !existing->created.empty() ? existing->created : isoUtc(host->wallNowMs());
            if (!writeLocked(p, e)) return err(500, e);
        }
        // Saving makes this the active profile and the current settings match it.
        std::string perr;
        Json patch = Json::object();
        patch["features"]["profiles"]["active"] = p.slug;
        backend->applyConfig(patch, perr);
        setWarning("");
        refresh();
        Json j = Json::object();
        j["ok"] = true; j["slug"] = p.slug; j["name"] = p.name;
        return reply(j);
    }

    HttpResponse doImport(const std::string& body) {
        Profile p;
        std::vector<std::string> warn;
        std::string e;
        if (!parseProfile(body, p, warn, e)) return err(422, e);
        {
            std::lock_guard<std::mutex> lk(opMu);
            const std::vector<Profile> list = scanLocked();
            if (list.size() >= kMaxProfiles) return err(409, "at most 100 profiles");
            p.slug = uniqueSlug(slugify(p.name), [&](const std::string& s) {
                for (auto& x : list) if (x.slug == s) return true;
                return false;
            });
            if (p.created.empty()) p.created = isoUtc(host->wallNowMs());
            if (!writeLocked(p, e)) return err(500, e);
        }
        refresh();
        Json j = Json::object();
        j["ok"] = true; j["slug"] = p.slug; j["name"] = p.name; j["warnings"] = strArray(warn);
        return reply(j);
    }

    HttpResponse doDelete(const std::string& slug) {
        {
            std::lock_guard<std::mutex> lk(opMu);
            struct stat st;
            const std::string path = pathOf(dir(), slug);
            if (::lstat(path.c_str(), &st) != 0) return err(404, "not found");
            if (::unlink(path.c_str()) != 0) return err(500, "cannot delete");
        }
        if (activeFromHost() == slug) {
            Json patch = Json::object();
            patch["features"]["profiles"]["active"] = Json();   // null deletes the key
            std::string pe;
            backend->applyConfig(patch, pe);
        }
        refresh();
        Json j = Json::object();
        j["ok"] = true;
        return reply(j);
    }

    HttpResponse doRename(const std::string& slug, const Json& b) {
        const Json& nm = b.get("name");
        if (nm.type() != Json::String || !validName(nm.asString())) return err(400, "name: 1 to 80 characters, no control characters, no leading or trailing space");
        std::string e;
        {
            std::lock_guard<std::mutex> lk(opMu);
            Profile p;
            std::vector<std::string> w;
            if (!loadLocked(slug, p, w, e)) return err(e == "not found" ? 404 : 422, e);
            p.name = nm.asString();                 // the slug stays: bindings and "active" refer to it
            if (!writeLocked(p, e)) return err(500, e);
        }
        refresh();
        Json j = Json::object();
        j["ok"] = true; j["slug"] = slug; j["name"] = nm.asString();
        return reply(j);
    }

    HttpResponse doDuplicate(const std::string& slug, const Json& b) {
        std::string e;
        Profile q;
        {
            std::lock_guard<std::mutex> lk(opMu);
            Profile p;
            std::vector<std::string> w;
            if (!loadLocked(slug, p, w, e)) return err(e == "not found" ? 404 : 422, e);
            std::string name = p.name + " copy";
            if (b.has("name")) {
                if (b.get("name").type() != Json::String || !validName(b.get("name").asString())) return err(400, "name: 1 to 80 characters, no control characters, no leading or trailing space");
                name = b.get("name").asString();
            } else if (name.size() > kMaxName) {
                std::string base = p.name.substr(0, kMaxName - 5);
                while (!base.empty() && (static_cast<unsigned char>(base.back()) & 0xC0) == 0x80) base.pop_back();   // do not cut a UTF-8 sequence
                if (!base.empty() && static_cast<unsigned char>(base.back()) >= 0xC0) base.pop_back();
                name = base + " copy";
            }
            const std::vector<Profile> list = scanLocked();
            if (list.size() >= kMaxProfiles) return err(409, "at most 100 profiles");
            q = p;
            q.name = name;
            q.slug = uniqueSlug(slugify(name), [&](const std::string& s) {
                for (auto& x : list) if (x.slug == s) return true;
                return false;
            });
            q.created = isoUtc(host->wallNowMs());
            if (!writeLocked(q, e)) return err(500, e);
        }
        refresh();
        Json j = Json::object();
        j["ok"] = true; j["slug"] = q.slug; j["name"] = q.name;
        return reply(j);
    }

    // ---- thread ----
    void loop() {
        int n = 0;
        std::unique_lock<std::mutex> lk(tmu);
        while (!stop) {
            tcv.wait_for(lk, std::chrono::milliseconds(opts.pollMs), [this] { return stop; });
            if (stop) break;
            lk.unlock();
            bool on;
            { std::lock_guard<std::mutex> g(mu); on = enabled; }
            if (on) {
                try { if (++n % 5 == 0) reindex(); refreshDirty(); } catch (...) {}
            }
            lk.lock();
        }
    }
};

ProfilesFeature::ProfilesFeature() : impl_(std::make_shared<Impl>()) {}
ProfilesFeature::ProfilesFeature(ProfilesOptions o) : impl_(std::make_shared<Impl>()) { impl_->opts = std::move(o); }
ProfilesFeature::~ProfilesFeature() { shutdown(); }

void ProfilesFeature::init(FeatureHost& host, const Json& s) {
    Impl& m = *impl_;
    m.host = &host;
    m.backend = m.opts.backend;
    if (!m.backend && host.hasCoreConfig()) m.backend = std::make_shared<CoreHostBackend>(host);
    m.realBackend = m.backend != nullptr;
    if (!m.backend) m.backend = std::make_shared<HostBackend>(host);
    { std::lock_guard<std::mutex> lk(m.mu); m.enabled = s.get("enabled").asBool(false); }
    if (m.enabled) m.refresh();
    if (m.opts.pollMs > 0 && !m.th.joinable()) m.th = std::thread([p = impl_] { p->loop(); });
}

void ProfilesFeature::applySettings(const Json& s) {
    Impl& m = *impl_;
    { std::lock_guard<std::mutex> lk(m.mu); m.enabled = s.get("enabled").asBool(false); }
    m.refresh();
}

void ProfilesFeature::refreshNow() { if (impl_->host) impl_->refresh(); }

Json ProfilesFeature::status() const {
    Impl& m = *impl_;
    Json j = Json::object();
    std::lock_guard<std::mutex> lk(m.mu);
    j["active"] = m.active;
    j["activeName"] = m.activeName;
    j["dirty"] = m.dirty;
    j["count"] = m.count;
    j["rev"] = double(m.rev);
    j["warning"] = m.warning;
    j["backend"] = m.realBackend ? "core" : "features-only";
    return j;
}

std::optional<HttpResponse> ProfilesFeature::handle(const HttpRequest& req, const std::string& subpath) {
    return impl_->handle(req, subpath);
}

void ProfilesFeature::shutdown() {
    Impl& m = *impl_;
    { std::lock_guard<std::mutex> lk(m.tmu); m.stop = true; }
    m.tcv.notify_all();
    if (m.th.joinable() && m.th.get_id() != std::this_thread::get_id()) m.th.join();
    if (m.host) {
        std::set<std::string> reg;
        { std::lock_guard<std::mutex> lk(m.mu); reg.swap(m.registered); }
        for (auto& id : reg) m.host->controls().remove(id);
    }
}

} // namespace shunt::app

SHUNT_REGISTER_FEATURE(shunt::app::ProfilesFeature);
