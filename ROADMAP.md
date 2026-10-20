# libhwsense Roadmap

## v0.4.1 (Current Release)

Packaging fixes for v0.4.0:

- `hwsense_unified.h` functions (`hwsense_read_all`, `hwsense_print_report`, `hwsense_get_*`) are now `HWSENSE_API`, so the **Windows DLL exports the unified API**. It was missing from the v0.4.0 DLL.
- The Linux library is built with hidden visibility, so internal helpers (`hwsense_ite_read_linux`, `hwsense_nct_decode`, ...) are no longer exported.
- The installed `hwsense.pc` is relocatable (`${pcfiledir}`) instead of hardcoding `/usr/local`, so the release tarball works from any directory.

---

## v0.4.0

### Added

| Feature | Notes |
|---------|-------|
| Linux sensors, mostly without root | CPU temperature via hwmon (AMD `k10temp`/`zenpower`, Intel `coretemp`; Intel MSR fallback), CPU clock/load, memory, uptime, GPU (NVML via `dlopen`, `amdgpu` hwmon), NVMe temperature |
| Unified API on Linux | `hwsense_read_all`, `hwsense_print_report` and JSON/CSV export now work on Linux |
| NVMe SMART | `hwsense_nvme_smart()` — wear, TB read/written, power-on hours, unsafe shutdowns, media errors. Needs root or `CAP_SYS_ADMIN`. Checked against `smartctl` |
| Super I/O fans, temps and voltages on Linux | From a loaded hwmon driver, or read directly over `/dev/port` (needs `CAP_SYS_RAWIO`) when none is loaded. Direct readers: ITE IT8655E/IT8665E/IT8686E and Nuvoton NCT6779/NCT6791-NCT6799 |
| `sensors_report` example | Prints every sensor available on the machine |

### Verification status

Verified on a Ryzen 5 4500 / RTX 4060 Ti / ITE IT8655E machine only. The Nuvoton direct reader is covered by simulated-register tests but has **not** been run on a real chip; Intel, `amdgpu` and IT8665E/IT8686E are likewise untested. See Known Gaps.

---

## v0.3.0

### Added

| Feature | Notes |
|---------|-------|
| Linux build target | Intel CPU temperature via `/dev/cpu/N/msr`, no kernel driver needed |
| JSON/CSV export | `hwsense_export_json` / `_csv_header` / `_csv_row`, snprintf-style buffer contract, RFC 8259 and RFC 4180 escaping |
| Threshold alerts | Edge-triggered with hysteresis, so no duplicate alerts and no flapping |
| Unit test suite | 130+ checks over register decode, serialization and alert logic — no hardware required, runs on both platforms in CI |
| Install + packaging | `find_package(hwsense)`, pkg-config on Linux, SONAME versioning |
| API reference | Doxygen, published to https://abduznik.github.io/libhwsense/ |
| CI | Windows + Linux build, test, install-and-consume, pkg-config metadata check |
| Zen4/Zen5 SMU entries | Raphael, Phoenix, Granite Ridge, Rembrandt — **unverified**, see below |

### Fixed

- AMD CPU families were compared in decimal against a CPUID value that lines up with the hex `15h`/`17h`/`19h` names, so **no Zen part was ever recognized** as a known family.
- `hwsense_cpu_core_voltage()` and `hwsense_cpu_package_power()` reported "not yet implemented" / "AMD-only" on Intel despite the MSR 0x198 and RAPL implementations existing.
- Sensor reads that failed on an unsupported CPU returned "Unknown CPU vendor" with nothing actionable; they now identify the part from CPUID.
- Vermeer `19h/20` and CastlePeak `17h/31` were recognized as codenames but had no SMU config entry, so those parts silently got none.
- `export.c` and `alert.c` were missing from the Linux source list, so the Linux library shipped headers declaring functions it did not contain.

### Carried over from earlier releases

CPU temperature/voltage/frequency/power (Intel MSR + AMD SMN/SVI2), GPU via NVML and ADL, NVMe SMART temperature, Super I/O, WMI queries, system stats, Python wrapper, unified API, CPU diagnostics.

---

## Known Gaps

**Unverified Zen4/Zen5 SMU offsets.** The Raphael, Phoenix, Granite Ridge and Rembrandt entries are transcribed from LibreHardwareMonitor and SMUDebugTool, not confirmed on real silicon. A wrong mailbox address returns plausible-looking garbage rather than failing, so suspect these offsets first if a reading looks wrong on one of those parts.

**Linux covers temperatures and system stats, not voltage or power.** Linux reads CPU temperature (AMD and Intel, via hwmon), clock, load, memory, GPU (NVML, `amdgpu`), NVMe temperature and Super I/O fans/temps/voltages (hwmon, or a direct ITE reader over `/dev/port`). CPU voltage and package power are still Windows-only on AMD (SMN/SVI2), as are the WMI, ADL and Embedded Controller backends. Verified on one machine only (Ryzen 5 4500, RTX 4060 Ti, ITE IT8655E, NVMe SMART checked against `smartctl`); the Intel path, `amdgpu`, the IT8665E/IT8686E direct reader and the Nuvoton direct reader (NCT6779-NCT6799) are untested on real hardware. The Nuvoton decode is only covered by a simulated register file built from the kernel `nct6775` tables.

**`HANDLE` in the public API.** `hwsense.h` still exposes Win32 types on a few entry points, guarded by `#ifdef _WIN32`. Decoupling these is a prerequisite for Linux being a first-class target.

---

## v0.5.0 (Next)

- **Network interface counters** — `win_get_net_io()` returns per-adapter rx/tx bytes, errors and link speed from the IP Helper API; no driver or admin needed (#3)
- **Process attribution** — `win_get_top_processes()` returns the top processes by working set or cumulative CPU time, so "the box is hot" can be answered with "this process is hot" (#16)
- **Verify the Linux path on more hardware** — Intel, `amdgpu`, IT8665E/IT8686E — and against `lm-sensors` (#37)
- **AMD on Linux** — temperature is done via hwmon; voltage and power (SMN/SVI2 via `/dev/cpu/N/msr` and PCI config) remain (#38)
- **OS abstraction layer** so vendor code stops carrying `#ifdef`s (#39)
- **Replace or augment WinRing0** (#44) — it is unmaintained, flagged by AV/EDR, and blocked by driver signature enforcement, which is a real adoption blocker
- Verify the Zen4/Zen5 SMU offsets on real parts

---

## Contributing

Check the [issues](https://github.com/abduznik/libhwsense/issues) for tasks tagged `good first issue` or `help wanted`.

Register addresses must be cited (Intel SDM, AMD PPR, or a named upstream implementation), and anything unverified on real hardware should say so in the source.
