// FS-02 scenario "key". Duration 30 s. Decks 1 and 2 are CDJ-3000 (status length 0x200,
// key bytes at 0x15c..0x15e).
//    0  deck 1 loads track 2001, deck 2 loads track 2002; deck 1 on air and playing, deck 2 cued
//   20  deck 2 loads track 2003
// The key encoding is undocumented (FS-02 task K0). Without --key-bytes the three tracks carry
// the PLACEHOLDER byte triples below, which only mean "different keys". With --key-bytes B0,B1,B2
// (hex, e.g. "8a,0,0" or "8a0000") every load writes exactly those bytes.
// Original Shunt code.
#include "Sim.h"

using namespace shuntsim;

namespace {

const uint8_t kPlaceholder[3][3] = {{0x8a, 0x00, 0x00}, {0x3b, 0x00, 0x00}, {0x9a, 0x00, 0x00}};

void key(Sim& s) {
    s.durationS = 30;
    auto bytes = [](Sim& s, int idx) { return s.opt.keyBytesSet ? s.opt.keyBytes : kPlaceholder[idx]; };
    s.at(0.0, [bytes](Sim& s) {
        s.loadTrack(1, 2001, bytes(s, 0));
        s.loadTrack(2, 2002, bytes(s, 1));
        s.setChannel(1, true); s.setChannel(2, false);
        s.deck(2).playing = false;
        s.log("deck 1 on air, deck 2 cued");
    });
    s.at(20.0, [bytes](Sim& s) { s.loadTrack(2, 2003, bytes(s, 2)); });
}

} // namespace

REGISTER_SCENARIO("key", key);
