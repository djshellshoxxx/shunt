// FS-06 scenario "wrongbar". Duration 30 s. The master (deck 1) reports beatInBar shifted by
// +2 (or --bar-shift N) relative to the true downbeat; nextBar timing and beatNumber stay true,
// so the true downbeat is the beat whose number is 1 mod 4.
// Original Shunt code.
#include "Sim.h"

using namespace shuntsim;

namespace {

void wrongbar(Sim& s) {
    s.durationS = 30;
    const int shift = s.opt.barShiftSet ? s.opt.barShift : 2;
    s.deck(1).derivedBar = true;
    s.deck(1).barShift = shift;
    s.deck(2).playing = false;
    s.at(0.0, [shift](Sim& s) { s.log("deck 1 master beatInBar shifted by %d", shift); });
}

} // namespace

REGISTER_SCENARIO("wrongbar", wrongbar);
