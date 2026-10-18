// blade - mid-asm hooks referenced from blade_manifest.toml ([[midasm_hook]]).
//
// Codegen emits `extern void <name>(...);` and calls the hook at the given
// guest address, so these must stay at global scope with these exact names.

// SEH funclet tail at 0x92164098 (see the manifest). The work is done by the
// manifest's `return = true`, which leaves the funclet instead of branching
// into its owner; the hook itself has nothing to do.
void BladeFuncletReturnToOwner() {}

// Screen Resolution "keep the new settings?" prompt at 0x9211BD04 (see the
// manifest). Returning true takes the manifest's jump_address_on_true, which
// skips it; the stock dashboard (BLADE_LAUNCHER=OFF) keeps it.
bool BladeSkipDisplayConfirm() { return BLADE_LAUNCHER; }
