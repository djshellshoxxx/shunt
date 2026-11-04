// FS-05 Replay a capture (docs/specs/FS-05-replay-a-capture.md): plays a pcapng from the captures
// folder through the whole stack (engine, tracklist, features, UI) using the FS-00 replay seam.
// Outputs stay off unless the user opts in (BUILD-PLAN R10); uploads are size limited (R12) and
// structurally validated; file names go through a strict whitelist. Original Shunt code.
//
// Locking (FS-00 section 1): ctlMu_ serialises HTTP control operations and is never taken under
// the Core lock, so handle() may call the host while holding it. mu_ guards the player state; it
// is taken inside step()/status() (Core lock held: Core -> mu_) and in handle() only around
// local state, never while calling the host. cacheMu_ is a leaf for the file-info cache.
#include "Replay.h"
#include "shunt/app/Feature.h"
#include "shunt/net/Packets.h"
#include "shunt/net/Socket.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace shunt::app::replay {

namespace {
constexpr uint32_t kShb = 0x0A0D0D0A, kIdb = 0x00000001, kEpb = 0x00000006;
constexpr uint32_t kByteOrder = 0x1A2B3C4D;
constexpr uint32_t kMaxBlockLen = 16u << 20;
constexpr int64_t kMinEngineNs = 1'000'000'000LL;

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

std::string mb(uint64_t bytes) { return std::to_string((bytes + (1u << 20) - 1) >> 20) + " MB"; }

// read(n, dst) reads the next n bytes sequentially; false at end of input.
template <class Read> PcapngCheck checkImpl(uint64_t size, uint64_t maxBytes, Read&& read) {
    PcapngCheck r;
    if (size > maxBytes) { r.error = "file is larger than " + mb(maxBytes); return r; }
    if (size < 28) { r.error = "too small to be a pcapng file"; return r; }
    std::vector<uint8_t> block;
    uint64_t pos = 0;
    while (pos < size) {
        const std::string at = " at offset " + std::to_string(pos);
        if (size - pos < 12) { r.error = "truncated block" + at; return r; }
        uint8_t hdr[8];
        if (!read(8, hdr)) { r.error = "read error" + at; return r; }
        const uint32_t type = le32(hdr), len = le32(hdr + 4);
        if (pos == 0) {
            if (type == 0xa1b2c3d4 || type == 0xd4c3b2a1 || type == 0xa1b23c4d || type == 0x4d3cb2a1) {
                r.error = "classic pcap file, not pcapng (convert with: editcap -F pcapng in out)";
                return r;
            }
            if (type != kShb) { r.error = "not a pcapng file (no section header block)"; return r; }
        }
        if (len < 12 || len % 4 != 0) { r.error = "bad block length " + std::to_string(len) + at; return r; }
        if (len > size - pos) { r.error = "truncated block" + at + " (needs " + std::to_string(len) + " bytes)"; return r; }
        if (len > kMaxBlockLen) { r.error = "block too large" + at; return r; }
        block.resize(len - 8);
        if (!read(len - 8, block.data())) { r.error = "read error" + at; return r; }
        const uint8_t* body = block.data();          // block body, then the trailing length
        const uint32_t bodyLen = len - 12;
        if (le32(body + bodyLen) != len) { r.error = "block length mismatch" + at; return r; }
        if (type == kShb) {
            if (bodyLen < 16) { r.error = "short section header" + at; return r; }
            const uint32_t magic = le32(body);
            if (magic == 0x4D3C2B1A) { r.error = "big-endian pcapng is not supported"; return r; }
            if (magic != kByteOrder) { r.error = "bad byte-order magic" + at; return r; }
        } else if (type == kIdb) {
            if (bodyLen < 8) { r.error = "short interface block" + at; return r; }
        } else if (type == kEpb) {
            if (bodyLen < 20) { r.error = "short packet block" + at; return r; }
            if (uint64_t(le32(body + 12)) > uint64_t(bodyLen) - 20) { r.error = "packet longer than its block" + at; return r; }
            ++r.packets;
        }
        if (++r.blocks > kMaxBlocks) { r.error = "too many blocks"; return r; }
        pos += len;
    }
    r.ok = true;
    return r;
}

void sortDevices(std::vector<SeenDevice>& d) {
    std::sort(d.begin(), d.end(), [](const SeenDevice& a, const SeenDevice& b) { return a.number < b.number; });
}
} // namespace

PcapngCheck checkPcapng(const uint8_t* data, size_t len, uint64_t maxBytes) {
    size_t pos = 0;
    return checkImpl(len, maxBytes, [&](size_t n, uint8_t* dst) {
        if (len - pos < n) return false;
        std::memcpy(dst, data + pos, n);
        pos += n;
        return true;
    });
}

PcapngCheck checkPcapngFile(const std::string& path, uint64_t maxBytes) {
    PcapngCheck r;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { r.error = "file not found"; return r; }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { r.error = std::string("cannot open: ") + std::strerror(errno); return r; }
    r = checkImpl(uint64_t(st.st_size), maxBytes, [&](size_t n, uint8_t* dst) { return std::fread(dst, 1, n, f) == n; });
    std::fclose(f);
    return r;
}

bool buildCapture(const std::vector<net::CapturedDatagram>& in, Capture& out, std::string& error) {
    out = Capture{};
    bool first = true;
    int64_t last = 0;
    std::map<int, std::string> devs;
    for (const auto& d : in) {
        if (d.dstPort < net::kAnnouncePort || d.dstPort > net::kStatusPort || d.payload.size() > 0xffff) { ++out.ignored; continue; }
        if (out.dgrams.size() >= kMaxDatagrams) { error = "more than " + std::to_string(kMaxDatagrams) + " datagrams"; out = Capture{}; return false; }
        if (first) { out.firstTsNs = d.timestampNs; first = false; }
        int64_t rel = d.timestampNs - out.firstTsNs;
        if (rel < last) rel = last;                   // out-of-order timestamps: never go back
        last = rel;
        Datagram g{rel, uint64_t(out.arena.size()), uint16_t(d.payload.size()), d.dstPort, d.srcIp};
        out.arena.insert(out.arena.end(), d.payload.begin(), d.payload.end());
        out.dgrams.push_back(g);
        if (d.dstPort == net::kAnnouncePort) {
            auto ev = net::parseAnnounce(d.payload.data(), d.payload.size(), 0, d.srcIp);
            if (ev && ev->type == net::EventType::KeepAlive && ev->keepAlive.number) {
                char name[sizeof ev->keepAlive.name + 1] = {};
                std::memcpy(name, ev->keepAlive.name, sizeof ev->keepAlive.name);
                devs[ev->keepAlive.number] = name;
            }
        }
    }
    if (out.dgrams.empty()) { error = "no player-network datagrams (UDP 50000-50002) in the capture"; return false; }
    out.durationNs = out.dgrams.back().relNs;
    for (auto& d : devs) out.devices.push_back({d.first, d.second});
    sortDevices(out.devices);
    return true;
}

bool loadCapture(const std::string& path, Capture& out, std::string& error, uint64_t maxBytes) {
    const PcapngCheck c = checkPcapngFile(path, maxBytes);
    if (!c.ok) { error = c.error; return false; }
    if (c.packets == 0) { error = "no packets in the capture"; return false; }
    if (c.packets > kMaxDatagrams * 4) { error = "too many packets"; return false; }
    std::vector<net::CapturedDatagram> all;
    {
        net::PcapngReader r;
        if (!r.open(path)) { error = "cannot read the capture"; return false; }
        while (auto d = r.next()) {
            if (d->dstPort < net::kAnnouncePort || d->dstPort > net::kStatusPort) continue;
            if (all.size() > kMaxDatagrams) break;     // buildCapture reports the cap
            all.push_back(std::move(*d));
        }
    }
    return buildCapture(all, out, error);
}

bool validName(const std::string& n) {
    static const std::string kExt = ".pcapng";
    if (n.size() <= kExt.size() || n.size() > kMaxNameLen || n[0] == '.') return false;
    for (unsigned char c : n)
        if (c >= 0x80 || !(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    return n.compare(n.size() - kExt.size(), kExt.size(), kExt) == 0;
}

std::string sanitizeName(const std::string& in) {
    std::string n;
    for (unsigned char c : in) {
        if (n.size() >= kMaxNameLen) break;
        n += c < 0x80 && (std::isalnum(c) || c == '.' || c == '_' || c == '-') ? char(c) : '_';
    }
    n.erase(0, n.find_first_not_of('.'));
    if (n.find_first_not_of('.') == std::string::npos) n.clear();
    return validName(n) ? n : std::string();
}

double snapSpeed(double s) {
    static const double kSpeeds[] = {0.25, 0.5, 1, 2, 4, 8};
    if (std::isnan(s)) return 1.0;
    if (s <= 0) return 0.25;
    if (std::isinf(s)) return 8.0;
    double best = 1, bestD = 1e9;
    for (double v : kSpeeds) {
        const double d = std::fabs(std::log2(s) - std::log2(v));
        if (d < bestD - 1e-9) { bestD = d; best = v; }
    }
    return best;
}

// ---- ReplayPlayer ------------------------------------------------------------------------------

void ReplayPlayer::load(std::shared_ptr<const Capture> cap, double speed, bool loop, int64_t nowNs) {
    cap_ = std::move(cap);
    speed_ = snapSpeed(speed);
    loop_ = loop;
    paused_ = ended_ = false;
    idx_ = 0;
    anchorPos_ = 0;
    anchorNow_ = nowNs;
    engineOffset_ = 0;
    pendingReset_ = cap_ != nullptr;
    loops_ = resets_ = fed_ = 0;
}

int64_t ReplayPlayer::positionNs(int64_t nowNs) const {
    if (!cap_) return 0;
    if (paused_ || ended_ || pendingReset_) return anchorPos_;
    const double adv = double(std::max<int64_t>(0, nowNs - anchorNow_)) * speed_;
    const double p = double(anchorPos_) + adv;
    return p >= double(cap_->durationNs) ? cap_->durationNs : int64_t(p);
}

void ReplayPlayer::pause(int64_t nowNs) {
    if (!cap_ || paused_ || ended_) return;
    anchorPos_ = positionNs(nowNs);
    anchorNow_ = nowNs;
    paused_ = true;
}

void ReplayPlayer::resume(int64_t nowNs) {
    if (!cap_ || !paused_) return;
    paused_ = false;
    anchorNow_ = nowNs;
}

void ReplayPlayer::seek(int64_t posNs, int64_t nowNs) {
    if (!cap_) return;
    anchorPos_ = std::clamp<int64_t>(posNs, 0, cap_->durationNs);
    anchorNow_ = nowNs;
    ended_ = false;
    pendingReset_ = true;
}

void ReplayPlayer::setSpeed(double speed, int64_t nowNs) {
    anchorPos_ = positionNs(nowNs);
    anchorNow_ = nowNs;
    speed_ = snapSpeed(speed);
}

void ReplayPlayer::doReset(int64_t targetNs, int64_t nowNs, ReplaySink& sink) {
    sink.reset(true);
    ++resets_;
    pendingReset_ = ended_ = false;
    const auto& d = cap_->dgrams;
    const int64_t warm = targetNs - kSeekWarmupNs;
    idx_ = warm <= 0 ? 0 : size_t(std::lower_bound(d.begin(), d.end(), warm, [](const Datagram& g, int64_t v) { return g.relNs < v; }) - d.begin());
    engineOffset_ = nowNs - targetNs;
    if (idx_ < d.size() && engineOffset_ + d[idx_].relNs < kMinEngineNs) engineOffset_ = kMinEngineNs - d[idx_].relNs;
    anchorPos_ = targetNs;
    anchorNow_ = nowNs;
}

size_t ReplayPlayer::step(int64_t nowNs, ReplaySink& sink, size_t budget) {
    if (!cap_) return 0;
    if (pendingReset_) doReset(anchorPos_, nowNs, sink);
    const auto& d = cap_->dgrams;
    const int64_t pos = positionNs(nowNs);
    size_t n = 0;
    while (idx_ < d.size() && d[idx_].relNs <= pos && n < budget) {
        const Datagram& g = d[idx_];
        const int64_t wallMs = (cap_->firstTsNs + g.relNs) / 1'000'000;
        sink.feed(g.port, cap_->arena.data() + g.off, g.len, engineOffset_ + g.relNs, g.srcIp, wallMs);
        ++idx_;
        ++n;
    }
    fed_ += n;
    if (idx_ >= d.size() && !paused_ && !ended_ && pos >= cap_->durationNs) {
        if (loop_ && cap_->durationNs > 0) {
            ++loops_;
            doReset(0, nowNs, sink);
        } else {
            ended_ = true;
            anchorPos_ = cap_->durationNs;
            anchorNow_ = nowNs;
        }
    }
    return n;
}

// ---- the feature ------------------------------------------------------------------------------

namespace {

HttpResponse jsonResp(const Json& j, int status = 200) {
    HttpResponse r;
    r.status = status;
    r.body = j.dump();
    return r;
}
HttpResponse err(int status, const std::string& msg) {
    Json j = Json::object();
    j["ok"] = false;
    j["error"] = msg;
    return jsonResp(j, status);
}

// Query values arrive percent-encoded (encodeURIComponent in the UI).
std::string urlDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') out += ' ';
        else if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(uint8_t(in[i + 1])) && std::isxdigit(uint8_t(in[i + 2]))) {
            out += char(std::stoi(in.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out += in[i];
    }
    return out;
}

std::string defaultCapturesDir() {
    const char* h = std::getenv("HOME");   // same default data folder as shunt_headless
    return (h && *h ? std::string(h) + "/.local/share/shunt" : std::string("shunt-data")) + "/captures";
}

bool mkdirs(const std::string& dir) {
    std::string cur;
    size_t p = 0;
    while (p != std::string::npos) {
        p = dir.find('/', p + 1);
        cur = dir.substr(0, p);
        if (cur.empty()) continue;
        if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    struct stat st{};
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

struct HostSink : ReplaySink {
    explicit HostSink(FeatureHost& h) : host(h) {}
    void feed(uint16_t port, const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp, int64_t wallMs) override {
        host.feed(port, data, len, recvNs, srcIp, wallMs);
    }
    void reset(bool clearSession) override { host.resetEngine(clearSession); }
    FeatureHost& host;
};

struct FileInfo {
    int64_t size = 0, mtimeNs = 0;
    Json info;
};

class ReplayFeature final : public IFeature, public ReplayHooks {
public:
    std::string id() const override { return "replay"; }

    void init(FeatureHost& h, const Json& s) override {
        host_ = &h;
        applyDir(s);
    }

    void applySettings(const Json& s) override {
        applyDir(s);
        if (!s.get("enabled").asBool(false)) {
            std::lock_guard<std::mutex> ctl(ctlMu_);
            stopLocked("feature disabled");
        }
    }

    void shutdown() override {
        std::lock_guard<std::mutex> ctl(ctlMu_);
        if (host_) stopLocked("");
    }

    Json status() const override {     // Core lock held: replaying() is lock-aware
        const bool hostReplaying = host_ && host_->replaying();
        std::lock_guard<std::mutex> lk(mu_);
        return statusLocked(hostReplaying, net::monotonicNowNs());
    }

    void step(int64_t nowNs) override {   // net thread, Core lock held
        std::lock_guard<std::mutex> lk(mu_);
        if (!active_ || !player_.loaded()) return;
        HostSink sink(*host_);
        player_.step(nowNs, sink);
        host_->setOutputsEnabled(outputs_);
    }

    std::optional<HttpResponse> handle(const HttpRequest& req, const std::string& sub) override {
        const bool get = req.method == "GET" || req.method == "HEAD", post = req.method == "POST";
        if (sub == "status" || sub == "files" || sub == "info") {
            if (!get) return err(405, "method not allowed");
            if (sub == "status") {
                const bool r = host_->replaying();
                std::lock_guard<std::mutex> lk(mu_);
                return jsonResp(statusLocked(r, net::monotonicNowNs()));
            }
            if (sub == "files") return files();
            return info(urlDecode(req.queryParam("file")));
        }
        static const char* kPost[] = {"start", "pause", "resume", "stop", "seek", "set", "upload"};
        if (std::find(std::begin(kPost), std::end(kPost), sub) == std::end(kPost)) return std::nullopt;
        if (!post) return err(405, "method not allowed");
        std::lock_guard<std::mutex> ctl(ctlMu_);
        if (sub == "upload") return upload(req);
        if (sub == "stop") { stopLocked(""); Json j = Json::object(); j["ok"] = true; return jsonResp(j); }
        Json body = Json::object();
        if (!req.body.empty()) {
            if (req.body.size() > 4096) return err(413, "body too large");
            std::string e;
            if (!Json::parse(req.body, body, &e)) return err(400, "invalid JSON: " + e);
            if (!body.isObject()) return err(400, "expected an object");
        }
        if (sub == "start") return start(body);
        syncExternalStop();
        std::lock_guard<std::mutex> lk(mu_);
        if (!active_) return err(409, "no replay is running");
        const int64_t now = net::monotonicNowNs();
        if (sub == "pause") player_.pause(now);
        else if (sub == "resume") {
            if (outputs_ && player_.paused()) player_.seek(player_.positionNs(now), now);   // realign outputs with the clock
            player_.resume(now);
        } else if (sub == "seek") {
            const Json& t = body.get("t");
            if (t.type() != Json::Number || !std::isfinite(t.asNumber())) return err(400, "t must be a number of seconds");
            const double s = std::clamp(t.asNumber(), 0.0, double(player_.durationNs()) / 1e9);
            player_.seek(int64_t(s * 1e9), now);
        } else {   // set
            const Json& sp = body.get("speed");
            const Json& lp = body.get("loop");
            if (!sp.isNull() && sp.type() != Json::Number) return err(400, "speed must be a number");
            if (!lp.isNull() && lp.type() != Json::Bool) return err(400, "loop must be true or false");
            if (!sp.isNull()) {
                const double v = snapSpeed(sp.asNumber());
                if (outputs_ && v != 1.0) return err(400, "speed must be 1 while outputs are on");
                player_.setSpeed(v, now);
            }
            if (!lp.isNull()) player_.setLoop(lp.asBool());
        }
        return jsonResp(statusLocked(true, now));
    }

private:
    void applyDir(const Json& s) {
        const Json& d = s.get("capturesDir");
        std::string dir = defaultCapturesDir();
        if (d.type() == Json::String && !d.asString().empty() && d.asString()[0] == '/' && d.asString().size() < 1024 &&
            d.asString().find('\0') == std::string::npos)
            dir = d.asString();
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        std::lock_guard<std::mutex> lk(mu_);
        dir_ = dir;
    }
    std::string dir() const { std::lock_guard<std::mutex> lk(mu_); return dir_; }
    bool enabled() const { return host_->settings("replay").get("enabled").asBool(false); }

    Json statusLocked(bool hostReplaying, int64_t now) const {
        Json j = Json::object();
        const bool active = active_ && hostReplaying && player_.loaded();
        j["active"] = active;
        j["file"] = active ? file_ : std::string();
        j["positionS"] = active ? double(player_.positionNs(now)) / 1e9 : 0.0;
        j["durationS"] = active ? double(player_.durationNs()) / 1e9 : 0.0;
        j["speed"] = player_.speed();
        j["paused"] = active && player_.paused();
        j["loop"] = player_.loop();
        j["ended"] = active && player_.ended();
        j["outputs"] = active && outputs_;
        j["loops"] = int64_t(player_.loops());
        j["datagrams"] = active ? int64_t(player_.capture()->dgrams.size()) : int64_t(0);
        j["fed"] = int64_t(player_.fed());
        j["lastError"] = lastError_;
        return j;
    }

    // The core can end a replay on its own (exception in step, Core::stop): forget ours then.
    void syncExternalStop() {
        const bool r = host_->replaying();
        std::lock_guard<std::mutex> lk(mu_);
        if (active_ && !r) { active_ = false; player_.unload(); lastError_ = "replay ended by the core"; }
    }

    void stopLocked(const std::string& why) {   // ctlMu_ held, mu_ not held
        host_->leaveReplay();
        std::lock_guard<std::mutex> lk(mu_);
        if (active_ && !why.empty()) lastError_ = why;
        active_ = false;
        outputs_ = false;
        player_.unload();
    }

    HttpResponse start(const Json& body) {
        if (!enabled()) return err(409, "enable the replay feature first (features.replay.enabled)");
        const Json& f = body.get("file");
        if (f.type() != Json::String || !validName(f.asString()))
            return err(400, "file must be a bare .pcapng name ([A-Za-z0-9._-], no path)");
        const Json& sp = body.get("speed");
        const Json& lp = body.get("loop");
        const Json& op = body.get("outputs");
        if (!sp.isNull() && sp.type() != Json::Number) return err(400, "speed must be a number");
        if (!lp.isNull() && lp.type() != Json::Bool) return err(400, "loop must be true or false");
        if (!op.isNull() && op.type() != Json::Bool) return err(400, "outputs must be true or false");
        const double speed = snapSpeed(sp.isNull() ? 1.0 : sp.asNumber());
        const bool outputs = op.asBool(false);
        if (outputs && speed != 1.0) return err(400, "outputs can only be sent at speed 1");
        const std::string name = f.asString();
        auto cap = std::make_shared<Capture>();
        std::string e;
        if (!loadCapture(dir() + "/" + name, *cap, e)) return err(e == "file not found" ? 404 : 422, name + ": " + e);
        rememberInfo(name, *cap);
        syncExternalStop();
        const bool already = host_->replaying();
        {
            std::lock_guard<std::mutex> lk(mu_);
            player_.load(std::move(cap), speed, lp.asBool(false), net::monotonicNowNs());
            file_ = name;
            outputs_ = outputs;
            active_ = true;
            lastError_.clear();
        }
        if (!already && !host_->enterReplay(this)) {
            std::lock_guard<std::mutex> lk(mu_);
            active_ = false;
            player_.unload();
            return err(503, "cannot start a replay now (core not running or another replay active)");
        }
        std::lock_guard<std::mutex> lk(mu_);
        return jsonResp(statusLocked(true, net::monotonicNowNs()));
    }

    Json infoJson(const Capture& c) const {
        Json j = Json::object();
        j["datagrams"] = int64_t(c.dgrams.size());
        j["ignored"] = int64_t(c.ignored);
        j["durationS"] = double(c.durationNs) / 1e9;
        Json d = Json::array();
        for (auto& x : c.devices) { Json o = Json::object(); o["number"] = x.number; o["name"] = x.name; d.push(o); }
        j["devices"] = d;
        j["error"] = "";
        return j;
    }

    bool statFile(const std::string& name, struct stat& st) const {
        return ::stat((dir() + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }
    static int64_t mtimeNs(const struct stat& st) { return int64_t(st.st_mtim.tv_sec) * 1'000'000'000LL + st.st_mtim.tv_nsec; }

    void rememberInfo(const std::string& name, const Capture& c) {
        struct stat st{};
        if (!statFile(name, st)) return;
        Json j = infoJson(c);
        std::lock_guard<std::mutex> lk(cacheMu_);
        if (cache_.size() > 256) cache_.clear();
        cache_[name] = FileInfo{int64_t(st.st_size), mtimeNs(st), j};
    }

    Json cachedInfo(const std::string& name, const struct stat& st) const {
        std::lock_guard<std::mutex> lk(cacheMu_);
        auto it = cache_.find(name);
        if (it != cache_.end() && it->second.size == int64_t(st.st_size) && it->second.mtimeNs == mtimeNs(st)) return it->second.info;
        return Json();
    }

    HttpResponse files() const {
        const std::string d = dir();
        Json list = Json::array();
        std::vector<std::string> names;
        if (DIR* dp = ::opendir(d.c_str())) {
            while (dirent* e = ::readdir(dp)) {
                if (names.size() >= 1000) break;
                if (validName(e->d_name)) names.push_back(e->d_name);
            }
            ::closedir(dp);
        }
        std::sort(names.begin(), names.end());
        for (auto& n : names) {
            struct stat st{};
            if (!statFile(n, st)) continue;
            Json o = Json::object();
            o["name"] = n;
            o["size"] = int64_t(st.st_size);
            o["mtimeMs"] = int64_t(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1'000'000;
            o["info"] = cachedInfo(n, st);
            list.push(o);
        }
        Json j = Json::object();
        j["dir"] = d;
        j["files"] = list;
        j["maxUploadBytes"] = int64_t(kMaxUploadBytes);
        return jsonResp(j);
    }

    HttpResponse info(const std::string& name) {
        if (!validName(name)) return err(400, "file must be a bare .pcapng name");
        struct stat st{};
        if (!statFile(name, st)) return err(404, "file not found");
        Json j = cachedInfo(name, st);
        if (j.isNull()) {
            Capture c;
            std::string e;
            if (loadCapture(dir() + "/" + name, c, e)) j = infoJson(c);
            else {
                j = Json::object();
                j["datagrams"] = 0; j["ignored"] = 0; j["durationS"] = 0.0; j["devices"] = Json::array(); j["error"] = e;
            }
            std::lock_guard<std::mutex> lk(cacheMu_);
            if (cache_.size() > 256) cache_.clear();
            cache_[name] = FileInfo{int64_t(st.st_size), mtimeNs(st), j};
        }
        Json o = Json::object();
        o["name"] = name;
        o["info"] = j;
        return jsonResp(o);
    }

    HttpResponse upload(const HttpRequest& req) {
        if (!enabled()) return err(409, "enable the replay feature first (features.replay.enabled)");
        const std::string name = sanitizeName(urlDecode(req.queryParam("name")));
        if (name.empty()) return err(400, "name must end in .pcapng (letters, digits, . _ - only)");
        if (req.body.size() > kMaxUploadBytes) return err(413, "upload larger than " + mb(kMaxUploadBytes));
        const PcapngCheck c = checkPcapng(reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size(), kMaxUploadBytes);
        if (!c.ok) return err(422, c.error);
        if (c.packets == 0) return err(422, "no packets in the capture");
        const std::string d = dir();
        if (!mkdirs(d)) return err(500, "cannot create the captures folder");
        const std::string path = d + "/" + name;
        struct stat st{};
        const bool exists = ::lstat(path.c_str(), &st) == 0;
        if (exists && req.queryParam("replace") != "1") return err(409, name + " exists; upload with replace=1 to overwrite");
        if (exists && !S_ISREG(st.st_mode)) return err(409, name + " is not a regular file");
        static int counter = 0;   // ctlMu_ held
        const std::string tmp = d + "/.upload-" + std::to_string(::getpid()) + "-" + std::to_string(++counter) + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return err(500, "cannot write the capture");
        const bool ok = std::fwrite(req.body.data(), 1, req.body.size(), f) == req.body.size() && std::fflush(f) == 0 && ::fsync(fileno(f)) == 0;
        if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
            std::remove(tmp.c_str());
            return err(500, "cannot write the capture");
        }
        { std::lock_guard<std::mutex> lk(cacheMu_); cache_.erase(name); }
        Json j = Json::object();
        j["ok"] = true;
        j["name"] = name;
        j["size"] = int64_t(req.body.size());
        j["packets"] = int64_t(c.packets);
        return jsonResp(j, 201);
    }

    FeatureHost* host_ = nullptr;
    std::mutex ctlMu_;
    mutable std::mutex mu_;
    ReplayPlayer player_;
    std::string dir_, file_, lastError_;
    bool active_ = false, outputs_ = false;
    mutable std::mutex cacheMu_;
    std::map<std::string, FileInfo> cache_;
};

} // namespace
} // namespace shunt::app::replay

namespace shunt::app {
using ReplayFeatureImpl = replay::ReplayFeature;
SHUNT_REGISTER_FEATURE(ReplayFeatureImpl);
} // namespace shunt::app
