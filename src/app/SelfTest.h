#pragma once

namespace devdatapod {

// devdatapod-selftest: prove the P2 model invariants without a GUI and without touching the
// real data/ directory. Everything runs inside a throwaway temp root.
//
// This is the compile-and-run evidence docs/ROADMAP.md P2 asks for on the parts that do not
// need a real redis: slot allocation never drifting across restart, at most one default per
// kind, a self-contradictory file loading frozen and refusing writes, and the role/port drift
// between services.json and ports.json healing back to one consistent state.
//
// Returns the process exit code: 0 only when every check passed.
int runSelfTest();

}
