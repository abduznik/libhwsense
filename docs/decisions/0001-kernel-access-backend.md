# 0001: Kernel access backend (WinRing0 vs PawnIO)

Status: Proposed
Issue: #44 (Replace/augment WinRing0 with a safer, more modern kernel access path)

## Context

libhwsense reads hardware registers from ring 0 through `WinRing0x64.sys`
(OpenHardwareMonitor's driver). The driver is a problem for adoption:

- **Security.** WinRing0 is an unrestricted ring-0 primitive that gives any
  local process raw MSR, PCI config, IO port and physical memory access. It is
  tracked as a vulnerable driver (CVE-2020-14979, `vulnerableDriver:WinNT/Winring0`).
- **Defender / AV.** Microsoft Defender flags it as `HackTool:Win32/WinRing0`.
  Some anti-cheat systems block it outright.
- **Maintenance.** OpenHardwareMonitor is unmaintained. Upstream LibreHardwareMonitor
  swapped WinRing0 for PawnIO in PR #1857 (merged Sept 2025, first released in
  v0.9.5). v0.9.4 was the last WinRing0-based release.

The primitives this codebase currently depends on, by call site:

| Primitive | IOCTL | Used by |
|-----------|-------|---------|
| MSR read | `IOCTL_OLS_READ_MSR` | `src/cpu/intel.c`, `src/cpu/amd_zen.c` |
| PCI config read/write | `IOCTL_OLS_READ/WRITE_PCI_CONFIG` | `src/cpu/amd_zen.c` (SMN, Zen) |
| Physical memory read | `IOCTL_OLS_READ_MEMORY` | `src/cpu/amd_smu.c` (SMU mailbox / PM table) |
| IO port read/write | `IOCTL_OLS_READ/WRITE_IO_PORT_*` | `src/motherboard/ec.c`, `src/motherboard/superio.c` |

## Options considered

### A. Keep WinRing0 as-is
Lowest effort, full coverage of the primitives above. Blocks adoption on
systems where Defender or EDR is enabled, and the driver is a known
vulnerable driver. Not acceptable as the long-term default.

### B. PawnIO (namazso)
A signed kernel driver (`PawnIO.sys`) plus signed, per-chip modules written in
Pawn. Userspace talks to the driver through `PawnIOLib.dll`. The driver is
signed by Microsoft through namazso, and modules are signed by the PawnIO
author's key. The driver only runs modules that pass signature verification.

Pros:
- Maintained, and already used by LibreHardwareMonitor (v0.9.5+), FanControl,
  OpenRGB, and others. This gives an existing signed driver and install flow.
- Narrow interface. Modules expose specific operations, not raw access.

Cons and risks:
- **Coverage is per module, not general.** PawnIO does not offer generic
  MSR/PCI/memory/IO access. For each primitive we need, a module must exist or
  be written. Known gaps reported upstream: the SMU and PM-table path needed
  a dedicated RyzenSMU module (used by ZenStates) and was not fully covered
  for all codenames. Read-only SMN support was enough for LHM but writes were
  added later.
- **Modules are signed by the PawnIO author.** A libhwsense module would need
  signing, or we'd depend on upstream modules matching our needs.
- **Install UX.** Users must install PawnIO separately (`winget install namazso.PawnIO`
  or the installer from pawnio.eu). Without it, backend init fails.
- **32-bit.** The PawnIO driver does not support 32-bit hosts. The user-mode
  library has an x86 issue upstream (LHM #1895). libhwsense's x64 target is fine.
- **Driver/module pairing.** Driver and modules must come from compatible
  release ranges. An old PawnIO install may reject newer modules.

### C. In-house signed driver
Would require our own code-signing certificate (EV, and a Microsoft submission
for kernel drivers) and a security review of a new ring-0 surface. Out of
scope for this project's size and risk budget. Rejected for now.

### D. Other existing drivers (OCCT ORing, RTCore64)
Not reviewed in depth. ORing is not yet publicly released as a signed
redistributable. RTCore64 is a known vulnerable driver. Not recommended.

## Decision

**Keep WinRing0 as the default backend for now. Add PawnIO as an optional
backend, selected at build time, and track the migration in a follow-up issue.**

Rationale: no PawnIO module yet covers all four primitives we need on the
hardware we test (Ryzen 5 4500 / Renoir). Replacing the default backend without
hardware verification would silently break sensor reads. Adding the backend
behind a build flag lets us verify it without affecting current users.

## Mitigation until migration

- README and USAGE: document the Defender exclusion and the security tradeoff.
  Already documented; keep it current.
- Ship WinRing0 only through the documented release artifact, not as an unpinned
  download. Pin the SHA-256 of `WinRing0x64.sys` in release notes.
- The driver service is stopped and deleted on teardown (`src/core/driver.c`,
  around line 210). Keep that behavior for any new backend.

## Follow-up work (not in this change)

1. Add a `HWSENSE_KERNEL_BACKEND` CMake option (`winring0` | `pawnio`), default `winring0`.
2. Implement a PawnIO backend behind a small interface for MSR, PCI config,
   IO port and memory read. Start with the MSR read used by `intel.c` and
   `amd_zen.c`, since it is the smallest module.
3. Verify each primitive on real hardware before enabling it for that sensor.
   Until verified, the PawnIO path should report that sensor as unsupported
   (using whatever status mechanism the API uses for that), not return
   plausible-looking garbage (see the SMU note in ROADMAP.md).
4. Open a tracking issue for items 1 to 3 and link it from #44.

## Unverified

- Current PawnIO module coverage for PCI config, IO port and memory reads
  against the upstream repo (`namazso/PawnIO.Modules`) was not checked
  module-by-module here.
- No PawnIO code has been written or tested in this repo.
