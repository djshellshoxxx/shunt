// MIDI learn (FS-04): pure binding table and learn model, plus the feature class. The pure
// parts (BindingTable, MidiLearnModel) take time as an argument and have no threads, sockets or
// JSON dependencies beyond Json (de)serialisation, so they are unit tested with synthetic time.
// Original Shunt code.
#pragma once
#include "shunt/app/Feature.h"
#include "shunt/out/MidiIn.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace shunt::app {

struct MidiBinding {
    enum Type { Note, Cc, Program };
    enum Mode { Trigger, Toggle, Absolute };
    std::string action;
    int channel = -1;         // 0..15, -1 = any
    Type type = Note;
    int number = 0;           // 0..127
    Mode mode = Trigger;
    bool high = false;        // CC trigger edge state (not persisted)
};

const char* midiTypeName(MidiBinding::Type t);     // "note" | "cc" | "program"
const char* midiModeName(MidiBinding::Mode m);     // "trigger" | "toggle" | "absolute"
std::string midiBindingText(const MidiBinding& b); // "Note 60, ch any" (channels shown 1..16)
bool midiKeysOverlap(const MidiBinding& a, const MidiBinding& b);
bool midiEligible(const out::MidiMessage& m);      // Note On/Off, CC, Program Change

struct MidiHit {
    std::string action;
    double value = 1.0;       // 0..1
};

class BindingTable {
public:
    static constexpr size_t kMax = 128;
    const std::vector<MidiBinding>& all() const { return b_; }
    size_t size() const { return b_.size(); }
    const MidiBinding* find(const std::string& action) const;
    // Bindings of OTHER actions whose key overlaps (same type/number, same channel or either "any").
    std::vector<std::string> conflictsWith(const MidiBinding& b) const;
    // Sets the binding of b.action (replacing its old one). False when the table is full.
    bool set(const MidiBinding& b);
    void removeOverlapping(const MidiBinding& b);       // drops other actions' overlapping bindings
    bool remove(const std::string& action);
    std::vector<MidiHit> match(const out::MidiMessage& m);   // updates CC edge state

    // Parsing is tolerant: invalid entries are skipped, overlapping duplicates resolve last-wins,
    // everything beyond kMax is dropped; each case adds a text to `warnings`.
    // `defaultMode(action)` supplies the mode when the entry has none.
    using ModeFn = std::function<MidiBinding::Mode(const std::string&)>;
    static BindingTable fromJson(const Json& arr, const ModeFn& defaultMode, std::vector<std::string>& warnings);
    Json toJson() const;

private:
    std::vector<MidiBinding> b_;
};

class MidiLearnModel {
public:
    static constexpr int64_t kLearnMs = 15000;
    struct Learning { std::string action; MidiBinding::Mode mode; bool replace; bool exactChannel; int64_t deadlineMs; };
    struct Conflict { MidiBinding binding; std::vector<std::string> with; };
    struct Last { bool valid = false; out::MidiMessage msg; int64_t atMs = 0; uint64_t seq = 0; };
    enum class Start { Learning, Bound, Full };
    struct Outcome { std::vector<MidiHit> hits; bool changed = false; };

    BindingTable table;

    Start startLearn(const std::string& action, MidiBinding::Mode mode, bool replace, bool exactChannel, int64_t nowMs);
    void cancel();
    void tick(int64_t nowMs);                          // expires an overdue learn
    Outcome onMessage(const out::MidiMessage& m, int64_t nowMs);

    const Learning* learning() const { return learning_ ? &*learning_ : nullptr; }
    const Conflict* conflict() const { return conflict_ ? &*conflict_ : nullptr; }
    const Last& last() const { return last_; }
    const std::string& lastLearn() const { return lastLearn_; }   // "", "bound", "timeout", "cancelled", "conflict"
    uint64_t rev() const { return rev_; }              // bumped on every table change
    void bump() { ++rev_; }

private:
    std::optional<Learning> learning_;
    std::optional<Conflict> conflict_;
    Last last_;
    std::string lastLearn_;
    uint64_t rev_ = 1;
};

// The feature. Backends are not injectable: tests use a FIFO as the "device" (openRawMidiIn
// accepts character devices and FIFOs).
class MidiLearn : public IFeature {
public:
    MidiLearn();
    ~MidiLearn() override;
    std::string id() const override { return "midilearn"; }
    void init(FeatureHost& host, const Json& settings) override;
    void applySettings(const Json& settings) override;
    Json status() const override;
    std::optional<HttpResponse> handle(const HttpRequest& req, const std::string& subpath) override;
    void shutdown() override;

private:
    void loadSettings(const Json& s);
    void restartInput();
    void stopInput();
    void onMidi(const out::MidiMessage& m);
    void persist();
    Json stateJson();
    Json learningJson(int64_t now) const;
    Json lastJson(int64_t now) const;
    MidiBinding::Mode defaultModeFor(const std::string& action) const;

    FeatureHost* host_ = nullptr;
    mutable std::mutex mu_;                 // model_, enabled_, port_, warnings_
    mutable std::mutex lifeMu_;                   // serialises start/stop of the reader (before mu_)
    std::mutex saveMu_;                     // serialises persist() (before mu_)
    MidiLearnModel model_;
    bool enabled_ = false;
    std::string port_;
    std::vector<std::string> warnings_;
    std::unique_ptr<out::MidiInThread> in_;   // guarded by lifeMu_
    std::atomic<uint64_t> messages_{0}, fired_{0}, unknown_{0};
};

} // namespace shunt::app
