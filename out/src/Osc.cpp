#include "shunt/out/Osc.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace shunt::out {

namespace {

void padString(std::vector<uint8_t>& o, const std::string& s) {
    o.insert(o.end(), s.begin(), s.end());
    o.push_back(0);
    while (o.size() % 4) o.push_back(0);
}

void putU32(std::vector<uint8_t>& o, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s));
}

} // namespace

std::vector<uint8_t> encodeOsc(const OscMessage& m) {
    std::vector<uint8_t> o;
    padString(o, m.address);
    std::string tags = ",";
    for (auto& a : m.args)
        tags += std::holds_alternative<int32_t>(a) ? 'i' : std::holds_alternative<float>(a) ? 'f' : 's';
    padString(o, tags);
    for (auto& a : m.args) {
        if (auto* i = std::get_if<int32_t>(&a)) putU32(o, uint32_t(*i));
        else if (auto* f = std::get_if<float>(&a)) { uint32_t u; std::memcpy(&u, f, 4); putU32(o, u); }
        else padString(o, std::get<std::string>(a));
    }
    return o;
}

std::vector<uint8_t> encodeOscBundle(const std::vector<OscMessage>& msgs) {
    std::vector<uint8_t> o;
    padString(o, "#bundle");
    putU32(o, 0);
    putU32(o, 1);   // immediate
    for (auto& m : msgs) {
        auto e = encodeOsc(m);
        putU32(o, uint32_t(e.size()));
        o.insert(o.end(), e.begin(), e.end());
    }
    return o;
}

void OscOutput::send(const std::vector<OscMessage>& msgs) {
    if (msgs.empty()) return;
    const auto data = msgs.size() == 1 ? encodeOsc(msgs[0]) : encodeOscBundle(msgs);
    status_.enabled = true;
    if (tx_.send(data.data(), data.size())) {
        status_.connected = true;
        status_.messagesSent += msgs.size();
    } else {
        status_.connected = false;
        status_.lastError = "send failed";
    }
}

void OscOutput::update(const clock::Timeline& tl, int64_t nowNs) {
    if (!usable(tl)) return;
    std::vector<OscMessage> out;

    if (policy_.shouldPublishTempo(tl, nowNs)) {
        const float bpm = float(tl.bpm);
        if (sendShunt()) out.push_back({"/shunt/bpm", {bpm}});
        switch (cfg_.profile) {
        case OscProfile::Resolume:
            out.push_back({"/composition/tempocontroller/tempo",
                           {float(std::clamp((tl.bpm - 20.0) / 480.0, 0.0, 1.0))}});
            break;
        case OscProfile::GrandMA3: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "Master 3.1 At BPM %.2f", tl.bpm);
            out.push_back({"/gma3/cmd", {std::string(buf)}});
            break;
        }
        case OscProfile::MagicQ: out.push_back({"/magicq/bpm", {bpm}}); break;
        default: break;
        }
    }

    if (int(tl.playing) != lastPlaying_) {
        lastPlaying_ = int(tl.playing);
        if (sendShunt()) out.push_back({"/shunt/transport", {int32_t(tl.playing ? 1 : 0)}});
    }

    // One beat message per beat, when now passes the beat time.
    if (tl.playing) {
        const double period = tl.periodNs();
        int64_t k = tl.beatIndex + int64_t(std::floor(double(nowNs - tl.beatOriginNs) / period));
        if (tl.timeOfBeat(k) > nowNs) --k;
        if (lastBeat_ == INT64_MIN) lastBeat_ = k;           // do not fire for a past beat on start
        else if (k > lastBeat_) {
            lastBeat_ = k;
            const int bib = ((tl.beatInBar - 1 + int(k - tl.beatIndex)) % 4 + 4) % 4 + 1;
            if (sendShunt()) out.push_back({"/shunt/beat", {int32_t(bib), int32_t(tl.barKnown ? 1 : 0)}});
            if (cfg_.profile == OscProfile::Resolume && tl.barKnown && bib == 1)
                out.push_back({"/composition/tempocontroller/resync", {int32_t(1)}});
        }
    }
    send(out);
}

void OscOutput::onEvent(const net::Event& ev) {
    std::vector<OscMessage> out;
    if (ev.type == net::EventType::MasterChanged) {
        const int32_t to = ev.masterChanged.to;
        if (sendShunt()) out.push_back({"/shunt/master", {to}});
        if (cfg_.profile == OscProfile::QLab && cfg_.qlabMasterCue > 0)
            out.push_back({"/cue/" + std::to_string(cfg_.qlabMasterCue) + "/start", {}});
    } else if (ev.type == net::EventType::OnAir && sendShunt()) {
        for (int i = 0; i < ev.onAir.channelCount && i < 6; ++i) {
            const uint8_t st = ev.onAir.channels[i];
            auto it = onAir_.find(uint8_t(i + 1));
            if (it != onAir_.end() && it->second == st) continue;
            onAir_[uint8_t(i + 1)] = st;
            out.push_back({"/shunt/onair", {int32_t(i + 1), int32_t(st)}});
        }
    }
    send(out);
}

void OscOutput::onTrackLoaded(uint8_t deck, const std::string& title, const std::string& artist, uint32_t id) {
    std::vector<OscMessage> out;
    if (sendShunt()) {
        out.push_back({"/shunt/track", {title, artist, int32_t(deck)}});
        out.push_back({"/shunt/track/id", {int32_t(id)}});
    }
    if (cfg_.profile == OscProfile::QLab && cfg_.qlabTrackCue > 0)
        out.push_back({"/cue/" + std::to_string(cfg_.qlabTrackCue) + "/start", {}});
    send(out);
}

} // namespace shunt::out
