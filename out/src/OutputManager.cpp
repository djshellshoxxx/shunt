#include "shunt/out/OutputManager.h"
#include "shunt/net/Socket.h"
#include <cmath>
#include <time.h>

namespace shunt::out {

namespace {
void sleepNs(int64_t ns) {
    if (ns <= 0) return;
    timespec ts{time_t(ns / 1'000'000'000LL), long(ns % 1'000'000'000LL)};
    nanosleep(&ts, nullptr);
}
}

OutputManager::OutputManager(TimelineSource src) : src_(std::move(src)), link_(makeLinkSdkSession()) {}
OutputManager::~OutputManager() { stop(); }

void OutputManager::configure(const OutputSettings& s) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool portChanged = s.midi.port != settings_.midi.port || s.midi.enabled != settings_.midi.enabled;
    settings_ = s;
    gen_.setOptions(s.midi.options);
    link_.setEnabled(s.link.enabled);
    osc_.configure(s.osc);
    if (portChanged && !portOverride_) openMidi();
}

void OutputManager::openMidi() {
    port_.reset();
    midiError_.clear();
    if (!settings_.midi.enabled) return;
    if (settings_.midi.port.empty()) { midiError_ = "no port selected"; return; }
    port_ = openRawMidiPort(settings_.midi.port, midiError_);
}

void OutputManager::setMidiPort(std::unique_ptr<IMidiPort> p) {
    std::lock_guard<std::mutex> lk(mu_);
    port_ = std::move(p);
    portOverride_ = true;
    midiError_.clear();
}

void OutputManager::setLinkSession(std::unique_ptr<ILinkSession> s) {
    std::lock_guard<std::mutex> lk(mu_);
    link_.setSession(std::move(s));
    link_.setEnabled(settings_.link.enabled);
}

void OutputManager::push(const OutputEvent& e) {
    std::lock_guard<std::mutex> lk(mu_);
    if (events_.size() < 256) events_.push_back(e);
}

void OutputManager::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void OutputManager::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void OutputManager::run() {
    std::vector<MidiMsg> msgs;
    while (running_) {
        const int64_t now = net::monotonicNowNs();
        const clock::Timeline tl = src_();
        std::unique_lock<std::mutex> lk(mu_);
        while (!events_.empty()) {
            const OutputEvent e = events_.front();
            events_.pop_front();
            if (e.kind == OutputEvent::Master) osc_.onMasterChanged(e.deck);
            else if (e.kind == OutputEvent::Track) osc_.onTrackLoaded(e.deck, e.id, e.title, e.artist);
            else osc_.onOnAir(e.deck, e.on);
        }
        osc_.poll(now, tl);
        link_.poll(now, tl);
        if (settings_.midi.enabled && port_) {
            msgs.clear();
            gen_.poll(now, 3'000'000, tl, msgs);
            for (const auto& m : msgs) {
                const int64_t wait = m.timeNs - net::monotonicNowNs();
                if (wait > 200'000) { lk.unlock(); sleepNs(wait - 150'000); lk.lock(); }
                while (net::monotonicNowNs() < m.timeNs) {}
                const int64_t at = net::monotonicNowNs();
                if (!port_ || !port_->write(m.bytes, m.len)) { midiError_ = "write failed"; continue; }
                ++midiSent_;
                if (m.bytes[0] == 0xF8) {
                    jitter_.push_back(double(at - m.timeNs) / 1000.0);
                    if (jitter_.size() > 512) jitter_.pop_front();
                }
            }
        }
        lk.unlock();
        sleepNs(1'000'000);
    }
}

OutputsStatus OutputManager::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    OutputsStatus st;
    st.link.enabled = link_.enabled();
    st.link.available = link_.available();
    st.link.connected = link_.enabled() && link_.available();
    st.link.error = settings_.link.enabled ? link_.error() : "";
    st.link.peers = double(link_.peers());
    st.link.detail = "quantum " + std::to_string(int(link_.lastQuantum()));
    st.link.sent = link_.softCorrections() + link_.hardCorrections();

    st.midi.enabled = settings_.midi.enabled;
    st.midi.connected = settings_.midi.enabled && port_ != nullptr && midiError_.empty();
    st.midi.error = settings_.midi.enabled ? midiError_ : "";
    st.midi.detail = port_ ? port_->name() : settings_.midi.port;
    st.midi.sent = midiSent_;
    if (!jitter_.empty()) {
        double s = 0;
        for (double j : jitter_) s += j * j;
        st.midi.jitterUs = std::sqrt(s / double(jitter_.size()));
    }
    st.osc.enabled = osc_.settings().enabled;
    st.osc.connected = osc_.connected();
    st.osc.error = osc_.settings().enabled ? osc_.error() : "";
    st.osc.detail = osc_.settings().host + ":" + std::to_string(osc_.settings().port);
    st.osc.sent = osc_.sent();
    return st;
}

} // namespace shunt::out
