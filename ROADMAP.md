# libhwsense Roadmap

## v0.3.0 (Current Release)

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

**Linux support is Intel temperature only.** The WinRing0, WMI, ADL, Super I/O and Embedded Controller backends are Windows-only. The Linux path also has not been validated against `lm-sensors` on real hardware yet — CI confirms it compiles, links and fails cleanly without MSR access, nothing more.

**`HANDLE` in the public API.** `hwsense.h` still exposes Win32 types on a few entry points, guarded by `#ifdef _WIN32`. Decoupling these is a prerequisite for Linux being a first-class target.

---

## v0.4.0 (Next)

- **Verify the Linux path on real hardware** against `lm-sensors` (#37)
- **AMD on Linux** — SMN/SVI2 via `/dev/cpu/N/msr` and PCI config (#38)
- **OS abstraction layer** so vendor code stops carrying `#ifdef`s (#39)
- **Replace or augment WinRing0** (#44) — it is unmaintained, flagged by AV/EDR, and blocked by driver signature enforcement, which is a real adoption blocker
- Verify the Zen4/Zen5 SMU offsets on real parts

---

## Contributing

Check the [issues](https://github.com/abduznik/libhwsense/issues) for tasks tagged `good first issue` or `help wanted`.

Register addresses must be cited (Intel SDM, AMD PPR, or a named upstream implementation), and anything unverified on real hardware should say so in the source.
