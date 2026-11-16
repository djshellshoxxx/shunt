// MIDI learn feature (FS-04): bind any ControlRegistry action to one MIDI message (Note, CC,
// Program Change), learn by pressing a control for 15 s, persist in settings.features.midilearn.
// Input comes from out::MidiInThread (raw device or FIFO); actions are invoked on that thread.
// Locking: lifeMu_ and saveMu_ before mu_; mu_ is never held while calling the host, the
// control registry or the reader thread. Original Shunt code.
#include "MidiLearn.h"
#include "shunt/out/Midi.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace shunt::app {

namespace {
int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
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
bool validActionId(const std::string& a) {
    if (a.empty() || a.size() > 128) return false;
    for (char c : a)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-')) return false;
    return true;
}
bool validPortPath(const std::string& p) {
    return p.empty() || (p.size() <= 256 && p[0] == '/' && p.find("..") == std::string::npos && p.find('\0') == std::string::npos);
}
bool parseType(const std::string& s, MidiBinding::Type& t) {
    if (s == "note") t = MidiBinding::Note;
    else if (s == "cc") t = MidiBinding::Cc;
    else if (s == "program") t = MidiBinding::Program;
    else return false;
    return true;
}
bool parseMode(const std::string& s, MidiBinding::Mode& m) {
    if (s == "trigger") m = MidiBinding::Trigger;
    else if (s == "toggle") m = MidiBinding::Toggle;
    else if (s == "absolute") m = MidiBinding::Absolute;
    else return false;
    return true;
}
bool isInt(const Json& j, int lo, int hi, int& out) {
    if (j.type() != Json::Number) return false;
    const double d = j.asNumber();
    if (d != std::floor(d) || d < lo || d > hi) return false;
    out = int(d);
    return true;
}
} // namespace

// ---- names and keys ----------------------------------------------------------------------------

const char* midiTypeName(MidiBinding::Type t) {
    switch (t) { case MidiBinding::Note: return "note"; case MidiBinding::Cc: return "cc"; default: return "program"; }
}
const char* midiModeName(MidiBinding::Mode m) {
    switch (m) { case MidiBinding::Trigger: return "trigger"; case MidiBinding::Toggle: return "toggle"; default: return "absolute"; }
}
std::string midiBindingText(const MidiBinding& b) {
    const char* t = b.type == MidiBinding::Note ? "Note" : b.type == MidiBinding::Cc ? "CC" : "Program";
    return std::string(t) + " " + std::to_string(b.number) + ", ch " + (b.channel < 0 ? std::string("any") : std::to_string(b.channel + 1));
}
bool midiKeysOverlap(const MidiBinding& a, const MidiBinding& b) {
    return a.type == b.type && a.number == b.number && (a.channel < 0 || b.channel < 0 || a.channel == b.channel);
}
bool midiEligible(const out::MidiMessage& m) {
    using K = out::MidiMessage;
    return m.kind == K::NoteOn || m.kind == K::NoteOff || m.kind == K::ControlChange || m.kind == K::ProgramChange;
}

// ---- BindingTable ------------------------------------------------------------------------------

const MidiBinding* BindingTable::find(const std::string& action) const {
    for (auto& b : b_) if (b.action == action) return &b;
    return nullptr;
}

std::vector<std::string> BindingTable::conflictsWith(const MidiBinding& nb) const {
    std::vector<std::string> v;
    for (auto& b : b_)
        if (b.action != nb.action && midiKeysOverlap(b, nb)) v.push_back(b.action);
    return v;
}

bool BindingTable::set(const MidiBinding& nb) {
    for (auto& b : b_)
        if (b.action == nb.action) { b = nb; b.high = false; return true; }
    if (b_.size() >= kMax) return false;
    b_.push_back(nb);
    b_.back().high = false;
    return true;
}

void BindingTable::removeOverlapping(const MidiBinding& nb) {
    b_.erase(std::remove_if(b_.begin(), b_.end(), [&](const MidiBinding& b) { return b.action != nb.action && midiKeysOverlap(b, nb); }), b_.end());
}

bool BindingTable::remove(const std::string& action) {
    const size_t n = b_.size();
    b_.erase(std::remove_if(b_.begin(), b_.end(), [&](const MidiBinding& b) { return b.action == action; }), b_.end());
    return b_.size() != n;
}

std::vector<MidiHit> BindingTable::match(const out::MidiMessage& m) {
    using K = out::MidiMessage;
    std::vector<MidiHit> hits;
    for (auto& b : b_) {
        if (b.channel >= 0 && b.channel != m.channel) continue;
        const bool abs = b.mode == MidiBinding::Absolute;
        switch (b.type) {
            case MidiBinding::Note:
                if (b.number != m.number) break;
                if (m.kind == K::NoteOn) hits.push_back({b.action, abs ? m.value / 127.0 : 1.0});
                else if (m.kind == K::NoteOff && abs) hits.push_back({b.action, 0.0});
                break;
            case MidiBinding::Cc:
                if (m.kind != K::ControlChange || b.number != m.number) break;
                if (abs) { hits.push_back({b.action, m.value / 127.0}); break; }
                if (m.value >= 64) {
                    if (!b.high) hits.push_back({b.action, 1.0});   // rising edge only
                    b.high = true;
                } else {
                    b.high = false;
                }
                break;
            case MidiBinding::Program:
                if (m.kind == K::ProgramChange && b.number == m.number) hits.push_back({b.action, abs ? m.number / 127.0 : 1.0});
                break;
        }
    }
    return hits;
}

BindingTable BindingTable::fromJson(const Json& arr, const ModeFn& defaultMode, std::vector<std::string>& warnings) {
    BindingTable t;
    if (arr.isNull()) return t;
    if (!arr.isArray()) { warnings.push_back("bindings is not an array, ignored"); return t; }
    size_t idx = 0;
    for (auto& e : arr.items()) {
        const size_t i = idx++;
        const std::string at = "binding " + std::to_string(i) + ": ";
        if (!e.isObject()) { warnings.push_back(at + "not an object"); continue; }
        MidiBinding b;
        b.action = e.get("action").asString("");
        if (!validActionId(b.action)) { warnings.push_back(at + "bad action id"); continue; }
        const Json& ch = e.get("channel");
        if (ch.isNull() || (ch.type() == Json::String && ch.asString() == "any")) b.channel = -1;
        else if (!isInt(ch, 0, 15, b.channel)) { warnings.push_back(at + "bad channel"); continue; }
        if (!parseType(e.get("type").asString(""), b.type)) { warnings.push_back(at + "bad type"); continue; }
        if (!isInt(e.get("number"), 0, 127, b.number)) { warnings.push_back(at + "bad number"); continue; }
        if (!parseMode(e.get("mode").asString(""), b.mode)) b.mode = defaultMode ? defaultMode(b.action) : MidiBinding::Trigger;
        if (t.b_.size() >= kMax && !t.find(b.action)) { warnings.push_back("more than 128 bindings, rest dropped"); break; }
        const auto clash = t.conflictsWith(b);
        if (!clash.empty()) {
            warnings.push_back(at + midiBindingText(b) + " also bound to " + clash.front() + ", last one wins");
            t.removeOverlapping(b);
        }
        if (t.find(b.action)) warnings.push_back(at + "action " + b.action + " bound twice, last one wins");
        t.set(b);
    }
    return t;
}

Json BindingTable::toJson() const {
    Json a = Json::array();
    for (auto& b : b_) {
        Json o = Json::object();
        o["action"] = b.action;
        if (b.channel < 0) o["channel"] = "any"; else o["channel"] = b.channel;
        o["type"] = midiTypeName(b.type);
        o["number"] = b.number;
        o["mode"] = midiModeName(b.mode);
        a.push(o);
    }
    return a;
}

// ---- MidiLearnModel ----------------------------------------------------------------------------

MidiLearnModel::Start MidiLearnModel::startLearn(const std::string& action, MidiBinding::Mode mode, bool replace, bool exactChannel, int64_t nowMs) {
    tick(nowMs);
    // Confirming a conflict that the previous learn ran into: apply the captured message now.
    if (conflict_ && conflict_->binding.action == action && replace) {
        MidiBinding b = conflict_->binding;
        conflict_.reset();
        learning_.reset();
        table.removeOverlapping(b);
        if (!table.set(b)) { lastLearn_ = "full"; return Start::Full; }
        lastLearn_ = "bound";
        ++rev_;
        return Start::Bound;
    }
    if (!table.find(action) && table.size() >= BindingTable::kMax) return Start::Full;
    conflict_.reset();
    learning_ = Learning{action, mode, replace, exactChannel, nowMs + kLearnMs};
    lastLearn_.clear();
    return Start::Learning;
}

void MidiLearnModel::cancel() {
    if (learning_) lastLearn_ = "cancelled";
    learning_.reset();
    conflict_.reset();
}

void MidiLearnModel::tick(int64_t nowMs) {
    if (learning_ && nowMs >= learning_->deadlineMs) { learning_.reset(); lastLearn_ = "timeout"; }
}

MidiLearnModel::Outcome MidiLearnModel::onMessage(const out::MidiMessage& m, int64_t nowMs) {
    Outcome o;
    tick(nowMs);
    if (midiEligible(m)) {
        last_.valid = true;
        last_.msg = m;
        last_.atMs = nowMs;
        ++last_.seq;
    }
    if (learning_) {
        if (!midiEligible(m)) return o;
        MidiBinding b;
        b.action = learning_->action;
        b.channel = learning_->exactChannel ? int(m.channel) : -1;
        b.type = m.kind == out::MidiMessage::ControlChange ? MidiBinding::Cc : m.kind == out::MidiMessage::ProgramChange ? MidiBinding::Program : MidiBinding::Note;
        b.number = m.number;
        b.mode = learning_->mode;
        const auto clash = table.conflictsWith(b);
        const bool replace = learning_->replace;
        learning_.reset();
        if (!clash.empty() && !replace) {
            conflict_ = Conflict{b, clash};
            lastLearn_ = "conflict";
            return o;
        }
        table.removeOverlapping(b);
        if (!table.set(b)) { lastLearn_ = "full"; return o; }
        lastLearn_ = "bound";
        ++rev_;
        o.changed = true;
        return o;
    }
    o.hits = table.match(m);
    return o;
}

// ---- feature -----------------------------------------------------------------------------------

MidiLearn::MidiLearn() = default;
MidiLearn::~MidiLearn() { stopInput(); }

MidiBinding::Mode MidiLearn::defaultModeFor(const std::string& action) const {
    if (host_)
        for (auto& a : host_->controls().list())
            if (a.id == action)
                return a.kind == ControlAction::Value ? MidiBinding::Absolute : a.kind == ControlAction::Toggle ? MidiBinding::Toggle : MidiBinding::Trigger;
    return MidiBinding::Trigger;
}

void MidiLearn::loadSettings(const Json& s) {
    std::vector<std::string> warn;
    BindingTable t = BindingTable::fromJson(s.get("bindings"), [this](const std::string& a) { return defaultModeFor(a); }, warn);
    std::string port = s.get("port").asString("");
    if (!validPortPath(port)) { warn.push_back("invalid port path ignored"); port.clear(); }
    std::lock_guard<std::mutex> lk(mu_);
    enabled_ = s.get("enabled").type() == Json::Bool ? s.get("enabled").asBool() : false;
    port_ = port;
    model_.cancel();
    model_.table = std::move(t);
    model_.bump();
    warnings_ = std::move(warn);
}

void MidiLearn::init(FeatureHost& host, const Json& settings) {
    host_ = &host;
    loadSettings(settings);
    restartInput();
}

void MidiLearn::applySettings(const Json& settings) {
    loadSettings(settings);
    restartInput();
}

void MidiLearn::shutdown() { stopInput(); }

void MidiLearn::stopInput() {
    std::unique_ptr<out::MidiInThread> old;
    {
        std::lock_guard<std::mutex> lk(lifeMu_);
        old = std::move(in_);
        if (old) old->stop();
    }
}

void MidiLearn::restartInput() {
    std::lock_guard<std::mutex> life(lifeMu_);
    if (in_) { in_->stop(); in_.reset(); }
    bool en;
    std::string port;
    { std::lock_guard<std::mutex> lk(mu_); en = enabled_; port = port_; }
    if (!en || port.empty()) return;
    in_ = std::make_unique<out::MidiInThread>(
        [port](std::string& e) { return out::openRawMidiIn(port, e); },
        [this](const out::MidiMessage& m) { onMidi(m); });
    in_->start();
}

void MidiLearn::onMidi(const out::MidiMessage& m) {
    MidiLearnModel::Outcome o;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!enabled_) return;
        ++messages_;
        o = model_.onMessage(m, steadyMs());
    }
    if (o.changed) persist();
    for (auto& h : o.hits) {
        if (host_->controls().invoke(h.action, h.value)) ++fired_; else ++unknown_;
    }
}

void MidiLearn::persist() {
    if (!host_) return;
    std::lock_guard<std::mutex> save(saveMu_);
    Json s = host_->settings("midilearn");
    {
        std::lock_guard<std::mutex> lk(mu_);
        s["schema"] = 1;
        s["port"] = port_;
        s["bindings"] = model_.table.toJson();
    }
    host_->saveSettings("midilearn", s);
}

Json MidiLearn::learningJson(int64_t now) const {
    const auto* l = model_.learning();
    if (!l) return Json();
    Json o = Json::object();
    o["action"] = l->action;
    o["secondsLeft"] = std::max<int64_t>(0, (l->deadlineMs - now + 999) / 1000);
    return o;
}

Json MidiLearn::lastJson(int64_t now) const {
    const auto& l = model_.last();
    if (!l.valid) return Json();
    Json o = Json::object();
    using K = out::MidiMessage;
    o["type"] = l.msg.kind == K::ControlChange ? "cc" : l.msg.kind == K::ProgramChange ? "program" : "note";
    o["on"] = l.msg.kind == K::NoteOn;
    o["channel"] = l.msg.channel;
    o["number"] = l.msg.number;
    o["value"] = l.msg.value;
    o["ageMs"] = std::max<int64_t>(0, now - l.atMs);
    o["seq"] = l.seq;
    return o;
}

Json MidiLearn::status() const {
    // Called with the Core lock held: own mutex only, no I/O.
    const int64_t now = steadyMs();
    bool connected = false;
    std::string error;
    {
        std::unique_lock<std::mutex> life(lifeMu_, std::try_to_lock);   // best effort, never blocks the net thread
        if (life.owns_lock() && in_) { connected = in_->connected(); error = in_->lastError(); }
    }
    std::lock_guard<std::mutex> lk(mu_);
    Json o = Json::object();
    o["port"] = port_;
    o["open"] = connected;
    o["inputError"] = error;
    o["learning"] = learningJson(now);
    o["lastMessage"] = lastJson(now);
    o["bindings"] = int(model_.table.size());
    o["rev"] = model_.rev();
    o["messages"] = messages_.load();
    return o;
}

Json MidiLearn::stateJson() {
    const int64_t now = steadyMs();
    std::vector<ControlAction> actions = host_->controls().list();
    std::set<std::string> known;
    for (auto& a : actions) known.insert(a.id);
    bool connected = false;
    std::string error;
    {
        std::lock_guard<std::mutex> life(lifeMu_);
        if (in_) { connected = in_->connected(); error = in_->lastError(); }
    }
    std::vector<out::MidiPortInfo> ports = out::listMidiPorts();
    std::lock_guard<std::mutex> lk(mu_);
    model_.tick(now);
    Json j = Json::object();
    j["enabled"] = enabled_;
    Json pa = Json::array();
    for (auto& p : ports) { Json o = Json::object(); o["id"] = p.id; o["name"] = p.name; pa.push(o); }
    j["ports"] = pa;
    j["port"] = port_;
    j["open"] = connected;
    j["inputError"] = error;
    j["learning"] = learningJson(now);
    j["lastLearn"] = model_.lastLearn();
    if (const auto* c = model_.conflict()) {
        Json o = Json::object();
        o["action"] = c->binding.action;
        o["text"] = midiBindingText(c->binding);
        Json w = Json::array();
        for (auto& s : c->with) w.push(s);
        o["with"] = w;
        j["conflict"] = o;
    } else {
        j["conflict"] = Json();
    }
    Json ba = Json::array();
    for (auto& b : model_.table.all()) {
        Json o = Json::object();
        o["action"] = b.action;
        if (b.channel < 0) o["channel"] = "any"; else o["channel"] = b.channel;
        o["type"] = midiTypeName(b.type);
        o["number"] = b.number;
        o["mode"] = midiModeName(b.mode);
        o["text"] = midiBindingText(b);
        o["missing"] = known.count(b.action) == 0;
        ba.push(o);
    }
    j["bindings"] = ba;
    Json aa = Json::array();
    for (auto& a : actions) {
        Json o = Json::object();
        o["id"] = a.id; o["label"] = a.label; o["kind"] = controlKindName(a.kind); o["group"] = a.group;
        aa.push(o);
    }
    j["actions"] = aa;
    j["lastMessage"] = lastJson(now);
    Json w = Json::array();
    for (auto& s : warnings_) w.push(s);
    j["warnings"] = w;
    return j;
}

std::optional<HttpResponse> MidiLearn::handle(const HttpRequest& req, const std::string& sub) {
    if (sub == "state") {
        if (req.method != "GET") return err(405, "method not allowed");
        return reply(stateJson());
    }
    if (sub != "learn" && sub != "cancel" && sub != "unbind" && sub != "port") return std::nullopt;
    if (req.method != "POST") return err(405, "method not allowed");
    if (req.body.size() > 4096) return err(413, "body too large");
    Json body = Json::object();
    if (!req.body.empty()) {
        std::string pe;
        if (!Json::parse(req.body, body, &pe) || !body.isObject()) return err(400, "body must be a JSON object");
    }

    if (sub == "cancel") {
        std::lock_guard<std::mutex> lk(mu_);
        model_.cancel();
        Json j = Json::object();
        j["ok"] = true;
        return reply(j);
    }

    if (sub == "unbind") {
        const std::string action = body.get("action").asString("");
        if (!validActionId(action)) return err(400, "bad action id");
        bool removed;
        {
            std::lock_guard<std::mutex> lk(mu_);
            removed = model_.table.remove(action);
            if (removed) model_.bump();
        }
        if (removed) persist();
        Json j = Json::object();
        j["ok"] = true;
        j["removed"] = removed;
        return reply(j);
    }

    if (sub == "port") {
        if (body.get("port").type() != Json::String) return err(400, "port must be a string");
        const std::string port = body.get("port").asString("");
        if (!validPortPath(port)) return err(400, "invalid port");
        if (!port.empty()) {
            bool listed = false;
            for (auto& p : out::listMidiPorts()) listed = listed || p.id == port;
            if (!listed) return err(400, "unknown port");
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            port_ = port;
            model_.bump();
        }
        persist();
        restartInput();
        Json j = Json::object();
        j["ok"] = true;
        j["port"] = port;
        return reply(j);
    }

    // learn
    const std::string action = body.get("action").asString("");
    if (!validActionId(action)) return err(400, "bad action id");
    if (body.has("replace") && body.get("replace").type() != Json::Bool) return err(400, "replace must be a boolean");
    if (body.has("exactChannel") && body.get("exactChannel").type() != Json::Bool) return err(400, "exactChannel must be a boolean");
    if (!host_->controls().has(action)) return err(404, "unknown action");
    const bool replace = body.get("replace").asBool(false);
    const bool exact = body.get("exactChannel").asBool(false);
    const MidiBinding::Mode mode = defaultModeFor(action);
    MidiLearnModel::Start st;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!enabled_) return err(409, "midilearn is disabled");
        st = model_.startLearn(action, mode, replace, exact, steadyMs());
    }
    if (st == MidiLearnModel::Start::Full) return err(409, "binding limit (128) reached");
    if (st == MidiLearnModel::Start::Bound) persist();
    Json j = Json::object();
    j["ok"] = true;
    j["bound"] = st == MidiLearnModel::Start::Bound;
    return reply(j);
}

SHUNT_REGISTER_FEATURE(MidiLearn);

} // namespace shunt::app
