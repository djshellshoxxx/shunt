// FS-01 scenarios. Decks 1 and 2 play, decks 3 and 4 are idle. Duration 45 s.
//   onair          : mixer on-air packets (0x03) drive channels, player F-byte on-air bit follows.
//   onair-nomixer  : same timeline but no 0x03 packets; only the player F-byte bits carry on-air.
// Timeline (seconds):
//    0  deck 1 master and on air (ch1), deck 2 cued off air
//   10  crossfade: ch1 off, ch2 on
//   12  deck 2 becomes master
//   30  all channels off
//   36  ch2 on again
//   45  end
// Original Shunt code.
#include "Sim.h"

using namespace shuntsim;

namespace {

void timeline(Sim& s) {
    s.durationS = 45;
    s.at(0.0, [](Sim& s) {
        s.setChannel(1, true); s.setChannel(2, false); s.setChannel(3, false); s.setChannel(4, false);
        s.log("deck 1 master and on air");
    });
    s.at(10.0, [](Sim& s) { s.setChannel(1, false); s.setChannel(2, true); s.log("crossfade to deck 2"); });
    s.at(12.0, [](Sim& s) { s.setMaster(2); });
    s.at(30.0, [](Sim& s) { for (int n = 1; n <= 4; ++n) s.setChannel(n, false); s.log("all channels off"); });
    s.at(36.0, [](Sim& s) { s.setChannel(2, true); });
}

void onair(Sim& s) { timeline(s); }
void onairNoMixer(Sim& s) { timeline(s); s.mixerOnAirPackets = false; }

} // namespace

REGISTER_SCENARIO("onair", onair);
REGISTER_SCENARIO("onair-nomixer", onairNoMixer);
