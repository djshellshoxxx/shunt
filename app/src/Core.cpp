#include "shunt/app/Core.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace shunt::app {

namespace {
int64_t wallNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
const char* stateName(clock::State s) {
    switch (s) {
    case clock::State::Idle: return "idle";
    case clock::State::Locking: return "locking";
    case clock::State::Locked: return "locked";
    case clock::State::Coasting: return "coasting";
    case clock::State::Paused: return "paused";
    case clock::State::MixerMaster: return "mixer";
    }
    return "idle";
}
const char* claimName(net::ClaimMachine::State s) {
    switch (s) {
    case net::ClaimMachine::State::Idle: return "idle";
    case net::ClaimMachine::State::Watching: return "watching";
    case net::ClaimMachine::State::Claiming: return "claiming";
    case net::ClaimMachine::State::Active: return "active";
    case net::ClaimMachine::State::Failed: return "failed";
    }
    return "idle";
}
const char* badgeFor(net::Model m) {
    switch (m) {
    case net::Model::Opus: return "tempo";
    case net::Model::PreNexus: return "partial";
    case net::Model::Unknown: return "unverified";
    case net::Model::AZ: return "unverified";
    default: return "full";
    }
}
void mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
}
std::string stamp(int64_t ms) {
    time_t t = time_t(ms / 1000);
    tm tmv{};
    gmtime_r(&t, &tmv);
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tmv);
    return b;
}
}

Core::Core(Settings s, std::string configPath, std::string dataDir)
    : settings_(std::move(s)), configPath_(std::move(configPath)), dataDir_(std::move(dataDir)),
      outputs_([this] { return timeline(); }) {
    factory_ = net::makePosixSocketFactory();
    applyEngineConfig();
    outputs_.configure(settings_.outputs);
    startNs_ = net::monotonicNowNs();
    sessionFile_ = dataDir_ + "/tracklists/session-" + stamp(wallNowMs()) + ".ndjson";
}

Core::~Core() { stop(); }

clock::Timeline Core::timeline() {
    std::lock_guard<std::mutex> lk(mu_);
    return tl_;
}

void Core::applyEngineConfig() {
    clock::Config c = engine_.config();
    c.latencyOffsetNs = int64_t(settings_.latencyMs * 1e6);
    c.userBarOffset = settings_.barOffset & 3;
    c.beatOnlyMode = settings_.beatOnly;
    c.profile = settings_.profile == "rehearsal" ? clock::Profile::Rehearsal : clock::Profile::Stage;
    engine_.setConfig(c);
}

bool Core::start(std::string* error) {
    if (running_.exchange(true)) return true;
    mkdirs(dataDir_ + "/tracklists");
    mkdirs(dataDir_ + "/captures");
    buildStack();
    if (!stackUp_ && error) *error = stackError_;
    outputs_.start();
    thread_ = std::thread([this] { netLoop(); });
    return true;
}

void Core::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    outputs_.stop();
    std::lock_guard<std::mutex> lk(mu_);
    session_.finish(wallNowMs());
    appendTracklistFilesLocked();
    if (stack_) stack_->stop();
}

void Core::buildStack() {
    std::lock_guard<std::mutex> lk(mu_);
    if (stack_) stack_->stop();
    stack_.reset();
    stackUp_ = false;
    stackError_.clear();
    firstRun_ = settings_.interfaceName.empty();
    auto ifaces = net::enumerateInterfaces();
    net::InterfaceInfo chosen;
    bool found = false;
    for (auto& i : ifaces) if (!settings_.interfaceName.empty() && (i.name == settings_.interfaceName || net::ipToString(i.address) == settings_.interfaceName)) { chosen = i; found = true; break; }
    if (!found && settings_.interfaceName.empty())
        for (auto& i : ifaces) if ((i.address >> 24) != 127) { chosen = i; found = true; break; }
    if (!found) { stackError_ = settings_.interfaceName.empty() ? "no network interface found" : "interface not found: " + settings_.interfaceName; return; }
    const bool loopback = (chosen.address >> 24) == 127;
    net::StackConfig cfg;
    cfg.mode = settings_.mode == "passive" ? net::Mode::Passive : net::Mode::Follow;
    cfg.address = chosen.address;
    cfg.netmask = chosen.netmask;
    cfg.broadcast = loopback ? chosen.address : chosen.broadcast;
    cfg.mac = chosen.mac;
    if (cfg.mac == std::array<uint8_t, 6>{}) cfg.mac = {0x02, 0x53, 0x48, 0x55, 0x4e, 0x54};
    cfg.leadNumber = uint8_t(settings_.deviceNumber);
    if (recording_) {
        capturePath_ = dataDir_ + "/captures/session-" + stamp(wallNowMs()) + ".pcapng";
        cfg.capturePath = capturePath_;
    }
    stack_ = std::make_unique<net::NetworkStack>(*factory_, cfg);
    if (!stack_->start()) {
        stackError_ = "cannot bind ports 50000-50002 (another player-network program running on this computer?)";
        stack_.reset();
        return;
    }
    ifaceInUse_ = chosen.name;
    timestampSource_ = stack_->timestampSource();
    stackUp_ = true;
}

void Core::netLoop() {
    while (running_) {
        if (restart_.exchange(false)) buildStack();
        net::NetworkStack* st;
        { std::lock_guard<std::mutex> lk(mu_); st = stack_.get(); }
        if (!st) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        st->poll(5);                       // only this thread touches the stack while it exists
        const int64_t now = net::monotonicNowNs();
        const int64_t wall = wallNowMs();
        std::lock_guard<std::mutex> lk(mu_);
        if (stack_.get() != st) continue;
        engine_.tick(now);
        net::Event ev;
        while (st->pop(ev)) handle(ev, wall);
        tl_ = engine_.timeline();
        devices_ = st->devices().devices();
        counters_ = st->counters();
        claimState_ = claimName(st->claim().state());
        claimNumber_ = st->claim().number();
        appendTracklistFilesLocked();
    }
}

void Core::handle(const net::Event& ev, int64_t wallMs) {
    engine_.onEvent(ev);
    Live& l = live_[ev.device];
    switch (ev.type) {
    case net::EventType::Beat:
        l.bpm = ev.beat.effectiveBpm; l.beatInBar = ev.beat.beatInBar; l.lastBeatNs = ev.recvTimeNs;
        if (ev.beat.isMaster) {
            const auto& tr = engine_.track(ev.device);
            if (tr.residualCount > 0) {
                scope_.push_back(tr.residualHistory[(tr.residualPos + 15) % 16] / 1e6);
                if (scope_.size() > 64) scope_.pop_front();
            }
        }
        break;
    case net::EventType::PlayerStatus: {
        const auto& s = ev.status;
        if (s.bpm100 != 0xffff) l.bpm = s.bpm100 * s.pitch1 / 100.0;
        l.pitchPct = (s.pitch1 - 1.0) * 100.0;
        l.playing = s.playing; l.master = s.master; l.onAir = s.onAir;
        const bool trackChanged = s.rekordboxId != l.trackId || s.slot != l.slot;
        l.trackId = s.rekordboxId; l.slot = s.slot; l.trackType = s.trackType;
        session_.onStatus(ev.device, s, wallMs);
        if (trackChanged && s.rekordboxId) outputs_.push({out::OutputEvent::Track, ev.device, s.rekordboxId, false, "", ""});
        if (ev.device == engine_.master() && (s.rekordboxId != masterTrackId_ || s.slot != masterTrackSlot_)) {
            masterTrackId_ = s.rekordboxId; masterTrackSlot_ = s.slot;
            const Json& mem = settings_.raw.get("barOffsetMemory").get(std::to_string(s.rekordboxId) + ":" + std::to_string(s.slot));
            if (s.rekordboxId && !mem.isNull()) { settings_.barOffset = mem.asInt() & 3; applyEngineConfig(); }
        }
        break;
    }
    case net::EventType::MixerStatus:
        l.master = ev.mixer.master;
        if (ev.mixer.bpm100 != 0xffff) l.bpm = ev.mixer.bpm100 / 100.0;
        break;
    case net::EventType::OnAir:
        for (int i = 0; i < ev.onAir.channelCount; ++i) {
            const bool on = ev.onAir.channels[i] != 0;
            Live& d = live_[uint8_t(i + 1)];
            if (d.onAir != on) outputs_.push({out::OutputEvent::OnAir, i + 1, 0, on, "", ""});
            d.onAir = on;
        }
        session_.onOnAir(ev.onAir, wallMs);
        break;
    case net::EventType::MasterChanged:
        outputs_.push({out::OutputEvent::Master, ev.masterChanged.to, 0, false, "", ""});
        for (auto& kv : live_) kv.second.master = kv.first == ev.masterChanged.to;
        break;
    case net::EventType::Device:
        if (ev.deviceEvent.kind == net::DeviceEvent::Left) live_.erase(ev.device);
        break;
    default: break;
    }
}

void Core::appendTracklistFilesLocked() {
    const auto& evs = session_.events();
    const auto& rows = session_.rows();
    if (writtenEvents_ >= evs.size() && writtenRows_ >= rows.size()) return;
    FILE* f = std::fopen(sessionFile_.c_str(), "a");
    if (!f) return;
    // Reuse the exporter for the new lines only.
    std::vector<log::TrackEvent> ne(evs.begin() + long(std::min(writtenEvents_, evs.size())), evs.end());
    std::vector<log::Row> nr(rows.begin() + long(std::min(writtenRows_, rows.size())), rows.end());
    const std::string text = log::toNdjson(ne, nr);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fflush(f);
    ::fsync(fileno(f));
    std::fclose(f);
    writtenEvents_ = evs.size();
    writtenRows_ = rows.size();
}

void Core::persistLocked() {
    settings_.save(configPath_);
}

// ---- controls ---------------------------------------------------------------------------------

void Core::setBarOffset(int offset) {
    std::lock_guard<std::mutex> lk(mu_);
    settings_.barOffset = offset & 3;
    applyEngineConfig();
    if (masterTrackId_) settings_.raw["barOffsetMemory"][std::to_string(masterTrackId_) + ":" + std::to_string(masterTrackSlot_)] = settings_.barOffset;
    persistLocked();
}

void Core::cycleBarOffset() { int cur; { std::lock_guard<std::mutex> lk(mu_); cur = settings_.barOffset; } setBarOffset(cur + 1); }

void Core::setBeatOnly(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    settings_.beatOnly = on;
    applyEngineConfig();
    persistLocked();
}

void Core::setLatencyMs(double ms) {
    std::lock_guard<std::mutex> lk(mu_);
    settings_.latencyMs = std::min(50.0, std::max(-20.0, ms));
    applyEngineConfig();
    persistLocked();
}

bool Core::latencyFromScope() {
    std::lock_guard<std::mutex> lk(mu_);
    if (scope_.size() < 8) return false;
    std::vector<double> v(scope_.begin(), scope_.end());
    std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
    settings_.latencyMs = std::min(50.0, std::max(-20.0, settings_.latencyMs + v[v.size() / 2]));
    applyEngineConfig();
    persistLocked();
    return true;
}

void Core::markSetStart() { std::lock_guard<std::mutex> lk(mu_); setStartMs_ = wallNowMs(); }

void Core::clearSession() {
    std::lock_guard<std::mutex> lk(mu_);
    session_.finish(wallNowMs());
    appendTracklistFilesLocked();
    session_ = log::TracklistSession();
    writtenEvents_ = writtenRows_ = 0;
    setStartMs_ = 0;
    sessionFile_ = dataDir_ + "/tracklists/session-" + stamp(wallNowMs()) + ".ndjson";
}

void Core::setRecording(bool on) {
    { std::lock_guard<std::mutex> lk(mu_); if (recording_ == on) return; recording_ = on; }
    restart_ = true;
}

bool Core::applyConfig(const Json& patch, std::string& error) {
    if (!patch.isObject()) { error = "expected an object"; return false; }
    bool restart = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Settings next = Settings::fromJson(patch, settings_);
        restart = next.interfaceName != settings_.interfaceName || next.mode != settings_.mode || next.deviceNumber != settings_.deviceNumber;
        settings_ = next;
        applyEngineConfig();
        outputs_.configure(settings_.outputs);
        if (!settings_.save(configPath_, &error)) return false;
    }
    if (restart) restart_ = true;
    return true;
}

// ---- JSON views -------------------------------------------------------------------------------

Json Core::config() {
    std::lock_guard<std::mutex> lk(mu_);
    return settings_.toJson();
}

Json Core::interfaces() {
    Json a = Json::array();
    for (auto& i : net::enumerateInterfaces()) {
        Json o = Json::object();
        o["name"] = i.name;
        o["ip"] = net::ipToString(i.address);
        char mac[24];
        std::snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", i.mac[0], i.mac[1], i.mac[2], i.mac[3], i.mac[4], i.mac[5]);
        o["mac"] = mac;
        o["loopback"] = (i.address >> 24) == 127;
        a.push(o);
    }
    return a;
}

Json Core::midiPorts() {
    Json a = Json::array();
    for (auto& p : out::listMidiPorts()) { Json o = Json::object(); o["id"] = p.id; o["name"] = p.name; a.push(o); }
    return a;
}

Json Core::status() {
    const out::OutputsStatus os = outputs_.status();
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t nowNs = net::monotonicNowNs();
    Json j = Json::object();
    j["type"] = "status";
    j["now"] = wallNowMs();
    j["uptimeS"] = double(nowNs - startNs_) / 1e9;
    j["mode"] = settings_.mode;
    j["interface"] = ifaceInUse_;
    j["firstRun"] = firstRun_;
    j["engineError"] = stackError_;
    j["timestampSource"] = timestampSource_;
    j["claim"] = claimState_;
    j["deviceNumber"] = claimNumber_;
    j["state"] = stateName(engine_.state());
    j["bpm"] = tl_.bpm;
    j["beatInBar"] = tl_.beatInBar;
    j["beatIndex"] = tl_.beatIndex;
    j["barKnown"] = tl_.barKnown;
    j["confidence"] = double(tl_.confidence);
    j["playing"] = tl_.playing;
    j["master"] = int(engine_.master());
    j["barOffset"] = settings_.barOffset;
    j["beatOnly"] = settings_.beatOnly;
    j["profile"] = settings_.profile;
    j["recording"] = recording_;
    j["setStartMs"] = setStartMs_;
    std::string masterName;
    Json devs = Json::array();
    for (auto& d : devices_) {
        Json o = Json::object();
        o["number"] = int(d.number);
        o["name"] = d.name;
        o["kind"] = d.kind == net::DeviceKind::Mixer ? "mixer" : d.kind == net::DeviceKind::Rekordbox ? "rekordbox" : "player";
        o["model"] = net::modelName(d.model);
        o["badge"] = badgeFor(d.model);
        o["ip"] = net::ipToString(d.ip);
        o["lastSeenMs"] = double(nowNs - d.lastKeepAliveNs) / 1e6;
        auto it = live_.find(d.number);
        if (it != live_.end()) {
            const Live& l = it->second;
            o["bpm"] = l.bpm; o["pitchPct"] = l.pitchPct; o["playing"] = l.playing; o["onAir"] = l.onAir;
            o["master"] = d.number == engine_.master();
            o["trackId"] = int64_t(l.trackId); o["slot"] = int(l.slot); o["beatInBar"] = l.beatInBar;
        }
        if (d.number == engine_.master()) masterName = d.name;
        devs.push(o);
    }
    j["devices"] = devs;
    j["masterName"] = masterName;

    Json outs = Json::object();
    auto outJson = [](const out::OutputStatus& s) {
        Json o = Json::object();
        o["enabled"] = s.enabled; o["available"] = s.available; o["connected"] = s.connected;
        o["error"] = s.error; o["detail"] = s.detail; o["peers"] = s.peers; o["jitterUs"] = s.jitterUs; o["sent"] = s.sent;
        return o;
    };
    outs["link"] = outJson(os.link);
    outs["midi"] = outJson(os.midi);
    outs["osc"] = outJson(os.osc);
    j["outputs"] = outs;

    Json sc = Json::object();
    Json res = Json::array();
    double sum = 0;
    std::vector<double> sorted(scope_.begin(), scope_.end());
    for (double r : scope_) { res.push(r); sum += r * r; }
    sc["residualsMs"] = res;
    sc["rmsMs"] = scope_.empty() ? 0.0 : std::sqrt(sum / double(scope_.size()));
    std::sort(sorted.begin(), sorted.end());
    sc["medianMs"] = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];
    sc["latencyMs"] = settings_.latencyMs;
    j["scope"] = sc;

    Json c = Json::object();
    c["datagrams"] = counters_.datagrams; c["parsed"] = counters_.parsed; c["parseErrors"] = counters_.parseErrors;
    c["filtered"] = counters_.filtered; c["sent"] = counters_.sent;
    Json types = Json::object();
    for (int i = 0; i < 256; ++i) if (counters_.perType[i]) { char k[8]; std::snprintf(k, sizeof k, "0x%02x", i); types[k] = counters_.perType[i]; }
    c["perType"] = types;
    j["counters"] = c;
    j["tracks"] = int(session_.rows().size());
    j["events"] = int(session_.events().size());
    return j;
}

Json Core::tracklist() {
    std::lock_guard<std::mutex> lk(mu_);
    Json rows = Json::array();
    for (auto& r : session_.rows()) {
        Json o = Json::object();
        o["startedAt"] = r.startedAtMs; o["endedAt"] = r.endedAtMs; o["deck"] = int(r.deck); o["slot"] = int(r.slot);
        o["rekordboxId"] = int64_t(r.rekordboxId); o["title"] = r.title; o["artist"] = r.artist;
        o["playedPct"] = r.playedPct; o["source"] = r.source;
        rows.push(o);
    }
    Json events = Json::array();
    const auto& evs = session_.events();
    for (size_t i = evs.size() > 100 ? evs.size() - 100 : 0; i < evs.size(); ++i) {
        Json o = Json::object();
        o["kind"] = log::eventKindName(evs[i].kind); o["at"] = evs[i].wallMs; o["deck"] = int(evs[i].deck);
        o["rekordboxId"] = int64_t(evs[i].rekordboxId);
        events.push(o);
    }
    Json j = Json::object();
    j["rows"] = rows;
    j["events"] = events;
    j["setStartMs"] = setStartMs_;
    return j;
}

bool Core::exportTracklist(const std::string& format, std::string& body, std::string& type, std::string& filename) {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t now = wallNowMs();
    // Include the rows still in progress without disturbing the live session.
    log::TracklistSession copy = session_;
    copy.finish(now);
    const int64_t setStart = setStartMs_ ? setStartMs_ : (copy.rows().empty() ? now : copy.rows().front().startedAtMs);
    const std::string base = "shunt-tracklist-" + stamp(now);
    if (format == "csv") { body = log::toCsv(copy.rows()); type = "text/csv"; filename = base + ".csv"; }
    else if (format == "cue") { body = log::toCue(copy.rows(), setStart, settings_.performer, settings_.venue.empty() ? "Shunt set" : settings_.venue); type = "application/x-cue"; filename = base + ".cue"; }
    else if (format == "txt") { body = log::toTimestampText(copy.rows(), setStart); type = "text/plain"; filename = base + ".txt"; }
    else if (format == "ndjson") { body = log::toNdjson(copy.events(), copy.rows()); type = "application/x-ndjson"; filename = base + ".ndjson"; }
    else if (format == "report") {
        char d[16]; time_t t = time_t(now / 1000); tm tmv{}; gmtime_r(&t, &tmv); std::strftime(d, sizeof d, "%Y-%m-%d", &tmv);
        body = log::toPerformanceReportCsv(copy.rows(), d, settings_.venue); type = "text/csv"; filename = base + "-report.csv";
    } else return false;
    return true;
}

Json Core::compat() {
    struct Row { const char *model, *badge, *note; };
    static const Row rows[] = {
        {"CDJ-3000 / 3000X", "full", "Players 1 to 6, precise position, phrase data on the network."},
        {"CDJ-2000NXS2 / TOUR1", "full", "Reference platform."},
        {"CDJ-2000NXS / 900NXS", "full", "Beats, status and on-air."},
        {"XDJ-1000 / 1000MK2 / XDJ-700", "full", "Cheapest genuine peers."},
        {"XDJ-XZ", "full", "Two decks, embedded mixer."},
        {"XDJ-AZ", "unverified", "Beats and master seen by the community; library is encrypted, tracklist is id only."},
        {"CDJ-2000 (non-nexus) / pre-nexus", "partial", "Works with less information."},
        {"Opus Quad", "tempo", "Tempo only: no beat packets, so bar alignment is impossible. Shown with a no-bar badge."},
        {"DJM-900NXS2 / V10 / A9 / TOUR1", "full", "On-air, master and BPM; follows as mixer master when no player is master."},
        {"DJM-750MK2", "partial", "Limited on the network; MIDI clock over USB, no Start/Stop."},
        {"XDJ-RX / RX2 / RX3", "unsupported", "The LINK port talks to rekordbox only; no player network in standalone."},
        {"DJM-S11 / Euphonia", "unsupported", "rekordbox and Serato only."},
        {"rekordbox (PC/Mac)", "full", "Performance mode as a peer. Cannot share a computer with Shunt in Follow mode (port conflict): use Passive."},
    };
    Json a = Json::array();
    for (auto& r : rows) { Json o = Json::object(); o["model"] = r.model; o["badge"] = r.badge; o["note"] = r.note; a.push(o); }
    return a;
}

} // namespace shunt::app
