// FS-05 / FS-08 scenario "longset": 20 minutes, 6 tracks (ids 3001..3006), two master handoffs,
// light jitter (0.3 ms) and 1 percent beat loss unless overridden. Use --time-scale to run fast.
//      0  deck 1 loads 3001 (master, playing); deck 2 loads 3002 (cued)
//    280  deck 2 starts playing
//    300  master handoff deck 1 -> deck 2 (announce, then switch at 300.4)
//    320  deck 1 stops; 330 deck 1 loads 3003
//    580  deck 1 starts playing
//    600  master handoff deck 2 -> deck 1
//    620  deck 2 stops; 630 deck 2 loads 3004
//    930  deck 1 loads 3005 (hot swap while playing)
//   1050  deck 2 loads 3006
//   1200  end
// Original Shunt code.
#include "Sim.h"

using namespace shuntsim;

namespace {

void handoffTo(Sim& s, int from, int to) {
    s.deck(from).handoffTo = uint8_t(to);
    s.log("deck %d announces handoff to deck %d", from, to);
}

void finishHandoff(Sim& s, int from, int to) {
    s.deck(from).master = false; s.deck(from).handoffTo = 0xff;
    s.deck(to).master = true;
    s.log("deck %d master", to);
}

void longset(Sim& s) {
    s.durationS = 1200;
    if (s.opt.jitterMs == 0) s.opt.jitterMs = 0.3;
    if (s.opt.lossPct == 0) s.opt.lossPct = 1;
    s.deck(2).playing = false;
    s.at(0.0, [](Sim& s) { s.loadTrack(1, 3001); s.loadTrack(2, 3002); });
    s.at(280.0, [](Sim& s) { s.deck(2).playing = true; s.deck(2).nextBeatNs = s.nowNs() + 100 * MS; s.log("deck 2 plays"); });
    s.at(300.0, [](Sim& s) { handoffTo(s, 1, 2); });
    s.at(300.4, [](Sim& s) { finishHandoff(s, 1, 2); });
    s.at(320.0, [](Sim& s) { s.deck(1).playing = false; s.log("deck 1 stops"); });
    s.at(330.0, [](Sim& s) { s.loadTrack(1, 3003); });
    s.at(580.0, [](Sim& s) { s.deck(1).playing = true; s.deck(1).nextBeatNs = s.nowNs() + 100 * MS; s.log("deck 1 plays"); });
    s.at(600.0, [](Sim& s) { handoffTo(s, 2, 1); });
    s.at(600.4, [](Sim& s) { finishHandoff(s, 2, 1); });
    s.at(620.0, [](Sim& s) { s.deck(2).playing = false; s.log("deck 2 stops"); });
    s.at(630.0, [](Sim& s) { s.loadTrack(2, 3004); });
    s.at(930.0, [](Sim& s) { s.loadTrack(1, 3005); });
    s.at(1050.0, [](Sim& s) { s.loadTrack(2, 3006); });
}

} // namespace

REGISTER_SCENARIO("longset", longset);
