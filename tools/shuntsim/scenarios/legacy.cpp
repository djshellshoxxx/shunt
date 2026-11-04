// Scenarios carried over unchanged from the original single-file simulator (RS-05):
// steady, handoff, nudge, pitch, hotcue, loop, pause, loss, jitter. All default to 20 s.
//   steady  : deck 1 master, decks 1 and 2 playing, nothing happens.
//   handoff : 6.0 deck 1 announces handoff to 2; 6.4 deck 2 becomes master.
//   nudge   : 6.0 deck 1 beat grid shifted 15 ms for 4 beats.
//   pitch   : 5.0 deck 1 pitch ramps +0.25 percent per beat for 8 beats.
//   hotcue  : 6.0 deck 1 jumps to beat 200, bar position 1, grid phase changes.
//   loop    : 6.0 deck 1 loops beats 9..12; 12.0 loop off.
//   pause   : 6.0 deck 1 stops; 11.0 deck 1 resumes mid-bar (beat 3) with a new phase.
//   loss    : steady with 3 percent beat loss (unless --loss given).
//   jitter  : steady with 5 ms jitter and 1 percent 100 ms outliers (unless given).
// Original Shunt code.
#include "Sim.h"

using namespace shuntsim;

namespace {

void steady(Sim&) {}

void handoff(Sim& s) {
    s.at(6.0, [](Sim& s) { s.deck(1).handoffTo = 2; s.log("deck 1 announces handoff to deck 2"); });
    s.at(6.4, [](Sim& s) { s.deck(1).master = false; s.deck(1).handoffTo = 0xff; s.deck(2).master = true; s.log("deck 2 master"); });
}

void nudge(Sim& s) {
    s.at(6.0, [](Sim& s) { s.deck(1).nextBeatNs += 15 * MS; s.nudgeBeats = 4; s.log("deck 1 grid nudged 15 ms"); });
}

void pitch(Sim& s) {
    s.at(5.0, [](Sim& s) { s.rampBeats = 8; s.log("deck 1 pitch ramp starts"); });
}

void hotcue(Sim& s) {
    s.at(6.0, [](Sim& s) {
        Deck& d = s.deck(1);
        d.nextBeatNs = s.nowNs() + int64_t(d.periodNs() * 0.37);
        d.beatNumber = 200; d.beatInBar = 1;
        s.log("deck 1 hot cue to beat 200");
    });
}

void loop(Sim& s) {
    s.at(6.0, [](Sim& s) { s.deck(1).loopOn = true; s.log("deck 1 loop on"); });
    s.at(12.0, [](Sim& s) { s.deck(1).loopOn = false; s.log("deck 1 loop off"); });
}

void pause(Sim& s) {
    s.at(6.0, [](Sim& s) { s.deck(1).playing = false; s.log("deck 1 stops"); });
    s.at(11.0, [](Sim& s) {
        Deck& d = s.deck(1);
        d.playing = true; d.nextBeatNs = s.nowNs() + 123 * MS; d.beatInBar = 3;
        s.log("deck 1 resumes at beat 3");
    });
}

void loss(Sim& s) { if (s.opt.lossPct == 0) s.opt.lossPct = 3; }

void jitter(Sim& s) {
    if (s.opt.jitterMs == 0) s.opt.jitterMs = 5;
    if (s.opt.outlierPct == 0) s.opt.outlierPct = 1;
}

} // namespace

REGISTER_SCENARIO("steady", steady);
REGISTER_SCENARIO("handoff", handoff);
REGISTER_SCENARIO("nudge", nudge);
REGISTER_SCENARIO("pitch", pitch);
REGISTER_SCENARIO("hotcue", hotcue);
REGISTER_SCENARIO("loop", loop);
REGISTER_SCENARIO("pause", pause);
REGISTER_SCENARIO("loss", loss);
REGISTER_SCENARIO("jitter", jitter);
