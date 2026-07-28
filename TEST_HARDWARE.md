# Test Hardware

Documents the hardware used to test and develop libhwsense.

## Primary Test System

| Component | Value |
|-----------|-------|
| **CPU** | AMD Ryzen 5 4500 (6-core, 12-thread) |
| **Architecture** | Zen 2 / Renoir |
| **Family/Model** | Family 17h Model 60h |
| **OS** | Windows (Admin) |
| **Python temp** | 47.5°C |
| **C temp** | 46.8°C (v0.1), 54.1°C (v0.2 with CCD support) |
| **CCD temps** | Not available (Renoir Model 0x60 — confirmed via SMN 0x00059954) |
| **Test date** | July 2026 |

Notes:
- AMD Renoir (Model 0x60) reports a single Tctl (package temperature), not per-core
- SMN register 0x00059800 reads the thermal sensor
- CCD temperatures at SMN 0x00059954/0x00059958 confirmed unavailable on Renoir
  (these are only documented for Matisse/Vermeer Desktop Zen 2/3)
- WinRing0x64.sys from OpenHardwareMonitor repo

## Future Test Hardware

Add rows here as you test on other systems:

| CPU | Architecture | Vendor | Status | Notes |
|-----|-------------|--------|--------|-------|
| AMD Ryzen 5 4500 | Zen 2 (Renoir) | AMD | Tested | Tctl via SMN 0x00059800 — 54.1°C |
| Intel Core i5-1235U | Alder Lake (12th Gen) | Intel | Tested | Per-core MSR 0x19C/0x1A2 — 57-75°C, TjMax=100 |
| AMD Ryzen 7 5800X | Zen 3 (Vermeer) | AMD | — | Desktop Zen 3 |
| Intel Core i7-10700 | Comet Lake | Intel | — | Desktop 10th Gen |

## Driver Version

| File | Source | Notes |
|------|--------|-------|
| `WinRing0x64.sys` | [OpenHardwareMonitor](https://github.com/openhardwaremonitor/openhardwaremonitor) repo, `Hardware/` dir | Checked into repo, always includes the driver |
| LibreHardwareMonitor v0.9.3 | Last version with WinRing0 bundled | v0.9.4+ switched to PawnIO driver |

## Build Environment

| Tool | Version |
|------|---------|
| MSVC | 19.44.35225.0 (VS 2022 Community) |
| CMake | via VS 2022 BuildTools |
| Python | 3.x (for the .py prototype) |
