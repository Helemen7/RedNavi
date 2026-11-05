// Navi 33 identity regression test; no kernel services or hardware I/O.
#include "navi33_facts.h"
#include <cassert>

using namespace rn_navi33;
static_assert(matches(kReference), "Reference IP Discovery identity must match");
static_assert(!kGpuWritesEnabled, "Navi 33 hardware writes must remain disabled");

int main() {
    assert(matches(kReference));

    Identity wrong = kReference;
    wrong.device = 0x747eu;  // Navi 32 is not Navi 33.
    assert(!matches(wrong));

    wrong = kReference;
    wrong.gc.revision = 3;
    assert(!matches(wrong));

    wrong = kReference;
    wrong.psp.revision = 10;
    assert(!matches(wrong));

    wrong = kReference;
    wrong.sdma.revision = 3;
    assert(!matches(wrong));

    wrong = kReference;
    wrong.dcn.revision = 0;
    assert(!matches(wrong));

    wrong = kReference;
    wrong.mmhub.revision = 0;
    assert(!matches(wrong));

    wrong = kReference;
    wrong.vendor = 0;
    assert(!matches(wrong));
    return 0;
}
