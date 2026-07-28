# /goal — Temperature Sensor Suite

## Goal

Build a lightweight hardware temperature monitoring library for the user's AMD Ryzen 5 4500 (Zen 2 / Renoir). Read all available temperature sensors via WinRing0 ring-0 access. No WMI, no heavy dependencies, just raw hardware registers.

## Hardware

| Component | Value |
|-----------|-------|
| CPU | AMD Ryzen 5 4500 |
| Arch | Zen 2 / Renoir (Family 17h Model 60h) |
| Cores | 6C / 12T |
| Driver | WinRing0x64.sys (OpenHardwareMonitor) |

## Available Temperature Sensors

### Via WinRing0 (ring-0)

| # | Sensor | Register | Type | Status |
|---|--------|----------|------|--------|
| 1 | CPU Package (Tctl) | SMN 0x00059800 | Package | **Done — 50.1 C** |
| 2 | CCD 0 temp | SMN 0x00059954 | Per-CCD | **Tested — unavailable on Renoir** |
| 3 | CCD 1 temp | SMN 0x00059958 | Per-CCD | **Tested — unavailable on Renoir** |
| 4 | CPU Core Voltage (VDDCR_CPU) | SMN 0x0005A010 (SVI2 Plane0) | Voltage | **Implemented — returns 0 on Cezanne APU (telemetry not exposed)** |
| 5 | SoC Voltage (VDDCR_SOC) | SMN 0x0005A00C (SVI2 Plane1) | Voltage | **Implemented — returns 0 on Cezanne APU (telemetry not exposed)** |
| 6 | CPU Package Power | SVI2 V*I estimate | Power | **Implemented — returns 0 on Cezanne APU (telemetry not exposed)** |

### Other Temperature Sources (future work)

| # | Sensor | Method | Difficulty |
|---|--------|--------|-----------|
| 4 | NVMe SSD temp | SMART via `IOCTL_STORAGE_QUERY_PROPERTY` | Easy |
| 5 | Motherboard temp | Super I/O chip via IO ports | Hard |
| 6 | GPU temp | NVML (NVIDIA) or ADL (AMD) | Medium |

## Build & Run

```powershell
# Build
cmake -S F:\Coding\sensors-test\libhwsense -B F:\Coding\sensors-test\libhwsense\build -G "Visual Studio 17 2022" -A x64
cmake --build F:\Coding\sensors-test\libhwsense\build --config Release

# Run (as Administrator)
F:\Coding\sensors-test\libhwsense\build\Release\read_cpu_temp.exe
```

## Milestones

- [x] **v0.1** — CPU Tctl via SMN (AMD) and MSR (Intel)
- [x] **v0.2** — CCD per-die temps — confirmed unavailable on Renoir (Family 17h Model 60h)
- [x] **v0.3** — AMD SVI2 voltages (Core VDD + SoC voltage) — implemented, SVI2 telemetry not exposed on Ryzen 5 4500
- [x] **v0.4** — Intel Core Voltage (MSR 0x198) + Package Power (RAPL sysfs) — tested on homelab i5-1235U
- [x] **v0.5** — AMD Package Power (SVI2 V*I estimate) — implemented, hardware-dependent
- [ ] **v0.6** — NVMe SSD temperature
- [ ] **v0.7** — Motherboard Super I/O temps (need chip ID)
- [ ] **v0.8** — GPU temperature (NVML or ADL)

## Notes

- AMD Renoir (Model 0x60) has fewer exposed sensors than Matisse (0x71)
- CCD temps at SMN 0x00059954 are documented for Matisse — need to test on Renoir
- The library is already very lightweight: one DeviceIoControl call per sensor reading
- No per-core temps on AMD — Tctl is package-level only (all cores share one reading)
