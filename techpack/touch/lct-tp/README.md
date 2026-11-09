# Spring LCT touch interface candidate

Reconstruction for the retained Spring 6.1.93 `lct_tp.ko`. This is not an
unmodified upstream driver. Declarations were recovered from the vendor_dlkm
module's DWARF; behavior was checked against the retained AArch64 code.
The vendor_boot and vendor_dlkm references have identical allocated sections
and normalized relocations. Debug information accounts for their size difference.

Reference SHA-256:
- vendor_boot: 13db9d15ab5879c0ee8ff798dbfc149ff26f34ce7a388b461a7675a77601b6bb
- vendor_dlkm: 3960cd601287cef1a29e1dc9c12aefa52fe790f7fadb4a1be62bbfea51d86a72

All 30 export names and recovered signatures are implemented. The recorded
reference CRCs are comparison data only. The build must generate its own CRCs;
no version-table patching or genksyms substitutions are used.

Behavior retained:
- Controller registration creates the eight individual proc interfaces.
- Info/lockdown reads call the registered refresh callback before returning text.
- Exported status setters update state without invoking the hardware callback.
- Proc toggle writes call the hardware callback only for a state transition.
- Self-test writes arm command IDs mmi=0, i2c=1, open=2, short=3. A subsequent
  initial read runs it. Result 1 retries at most three times; 2 is success;
  other results become 0. Unarmed reads return the previous result.
- A self-test write preserves the file offset for same-descriptor write/read use.
- A valid numeric proximity-data write resets the data to zero, as in the binary.
- Grip proc writes validate angles 0..360; a negative get/set callback result
  becomes EIO. Exported angle setters only update cached state.
- Proc modes match the reference, including read-only work/gesture nodes.

Safety changes:
- Static per-feature state removes the old dynamic-buffer lifetime hazards.
- Bounded text copies and input parsing replace unbounded string operations.
- Duplicate registration returns EBUSY without replacing an existing owner.
- Proc operations serialize per feature. Pending self-tests reject a second
  command with EBUSY rather than cancelling or replacing the armed command.
- Unregister blocks new callback dispatch, removes the proc entry, and waits
  for an SRCU grace period before clearing provider callbacks.

Captured live baseline:
- tp_info: [Vendor]csot,[FW]0x1a,[IC]nt36672s
- tp_lockdown_info: 534232304f196900
- tp_work=1, tp_proximity_data=0, tp_proximity_switch=0, tp_gesture=1
- tp_grip_area: EIO already occurred with the retained module.

Validation completed locally:
- Compiled the actual candidate C against a host kernel-API test shim with GCC,
  AddressSanitizer and UndefinedBehaviorSanitizer.
- Exercised metadata callbacks, truncation, self-test dispatch/retries and
  same-descriptor offsets, status setter/callback separation, proximity reset,
  negative grip callbacks, malformed/oversized writes and registration failure.
- A two-thread test verified unregister waits for an in-flight hardware callback.
- Checked all 30 reference export names are present.
These are host tests with kernel API stubs, not an ARM64 kernel or device test.
Kernel build, import audit, all 30 export CRC checks, and on-device tests remain.
Do not package or flash this source before those build/ABI checks pass.
