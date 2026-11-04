// FS-03 / FS-05 scenario "phrase". Duration 60 s. Deck 1 (128 BPM, master) plays; beatInBar
// is derived from the beat number so downbeats are beats 1, 5, 9...
//    0  beat number advances from 1
//   20  hot cue: jump back 32 beats (grid phase changes)
//   40  loop of 4 beats on
//   48  loop off (8 s of looping)
// Original Shunt code.
#include "Sim.h"

using namespace shuntsim;

namespace {

void phrase(Sim& s) {
    s.durationS = 60;
    s.deck(1).derivedBar = true;
    s.deck(2).playing = false;
    s.at(0.0, [](Sim& s) { s.setChannel(1, true); s.log("deck 1 plays from beat 1"); });
    s.at(20.0, [](Sim& s) {
        Deck& d = s.deck(1);
        d.beatNumber = d.beatNumber > 32 ? d.beatNumber - 32 : 1;
        d.beatInBar = int((d.beatNumber - 1) % 4) + 1;
        d.nextBeatNs = s.nowNs() + int64_t(d.periodNs() * 0.37);
        s.log("deck 1 hot cue back 32 beats to beat %u", d.beatNumber);
    });
    s.at(40.0, [](Sim& s) {
        Deck& d = s.deck(1);
        d.loopStart = d.beatNumber; d.loopLen = 4;
        s.log("deck 1 loop 4 beats from beat %u", d.beatNumber);
    });
    s.at(48.0, [](Sim& s) { s.deck(1).loopLen = 0; s.log("deck 1 loop off"); });
}

} // namespace

REGISTER_SCENARIO("phrase", phrase);
