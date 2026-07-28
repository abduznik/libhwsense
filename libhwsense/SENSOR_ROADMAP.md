# Sensor Roadmap

New sensors to add to libhwsense, ordered by difficulty and impact.

## Easy (just new MSR reads)

### CPU Core Voltage

| CPU | MSR | Bits | Formula |
|-----|-----|------|---------|
| Intel | `0x198` (IA32_PERF_STATUS) | EDX [15:0] | voltage = VID / 8192.0 |
| AMD | SMN `0x0005A010` (SVI2 Plane0) | [24:16] | voltage = VID * 0.00625 |

Core voltage is already available via the same DeviceIoControl path we use for temperature. Just different register addresses.

**AMD: Done** — `hwsense_cpu_core_voltage()` reads SVI2 Plane0, returns volts + amps.
**Intel: Done** — MSR 0x198, EDX[15:0] = VID. Tested on i5-1235U (Linux: `/dev/cpu/N/msr`).

### CPU Core Clocks

| CPU | MSR | Bits | Formula |
|-----|-----|------|---------|
| Intel | `0x198` (IA32_PERF_STATUS) | EAX [15:8] | multiplier = EAX[15:8]; clock = multiplier * bus_freq |
| AMD | SMN (PM table) or `0x0005A00C` (SVI2) | varies | SMU PM table index 48/50/51 = fabric/uncore/memory clocks |

Clock reading is useful for detecting turbo boost and power throttling.

### CPU Package Power

| CPU | MSR | Bits | Formula |
|-----|-----|------|---------|
| Intel | `0x610` (MSR_PKG_ENERGY_STATUS) | [31:0] | energy_delta / time_delta = power (W) |
| Intel | `0x606` (MSR_RAPL_POWER_UNIT) | [3:0] | energy_unit = 1 / (2^N) Joules |
| AMD | SMU PM table index 115 | float | SoC temperature (not power — need PM table access for power) |

Intel RAPL is straightforward (MSR-based). AMD power requires the SMU PM table transfer protocol (command 0x05 + DRAM base address) — more involved.

**Intel: Done** — Tested on i5-1235U (Linux). Note: direct MSR reads (0x610) blocked when `intel_rapl_msr` kernel module is loaded. Use sysfs `/sys/class/powercap/intel-rapl:0/energy_uj` instead. Package power = 5.42 W at idle.

## Medium (PCI config / SMN)

### AMD SVI2 Voltages — Partial (hardware-dependent)

AMD Family 17h+ telemetry via SMN registers:

| Register | SMN Address | Purpose | Status |
|----------|-------------|---------|--------|
| SVI0_TELEM_SVI | `0x0005A008` | Telemetry status (enable/disable bits) | Defined |
| SVI0_PLANE0 | `0x0005A010` | Core VDD current + voltage [24:16]=VDDCOR, [7:0]=IDDCOR | **Implemented** |
| SVI0_PLANE1 | `0x0005A00C` | SoC voltage + current | **Implemented** |

Same SMN access pattern as temperature (PCI config 0x60/0x64 on Bus 0/Dev 0/Func 0).
Implemented: `hwsense_cpu_core_voltage()` (Plane0) and `hwsense_amd_soc_voltage_dispatch()` (Plane1).

**Hardware limitation**: SVI2 telemetry registers return 0x00000000 on some CPU models (e.g., Ryzen 5 4500 / Cezanne APU). The registers are defined but not populated by the hardware. Works on Matisse (desktop Zen2) but not on all APUs.

### AMD SMU PM Table

The SMU PM table contains detailed per-core temps, clocks, power, and SoC sensors. Protocol:

1. Get PM table version (command 0x08)
2. Transfer PM table to DRAM (command 0x05)
3. Get DRAM base address (command 0x06)
4. Read DRAM at offset -> float array with sensor data

Requires the full SMU mailbox protocol (CMD/RSP/ARG registers via PCI config).

**Status**: SMU mailbox communication implemented in `amd_zen.c` (`smu_send_command()`).
PM table transfer and DRAM readback not implemented — WinRing0 doesn't support physical memory mapping needed to read the PM table from DRAM. Would require a different driver (RTCore64 or custom).

**Alternative**: Package power estimated via SVI2 telemetry: `P = V_core * I_core + V_soc * I_soc`.
Implemented in `hwsense_amd_package_power()`. Returns 0 on CPUs where SVI2 telemetry is not exposed (e.g., Cezanne APU).

## Hard (IO port access)

### Super I/O Voltages and Fan Speeds

Motherboard monitoring chips (Winbond NCT6775, Nuvoton NCT6798, ITE IT8688) expose voltages and fan speeds through IO ports.

| Chip | Address Port | Data Port | Access |
|------|-------------|-----------|--------|
| NCT6775 | 0x2E / 0x4E | 0x2F / 0x4F | Select register via index, read via data |
| NCT6798 | 0x4E | 0x4F | Same as above |

WinRing0 supports `IOCTL_OLS_READ_IO_PORT_BYTE` / `IOCTL_OLS_WRITE_IO_PORT_BYTE`:
- IOCTL code: `0x9C4020CC` (read) / `0x9C4020D0` (write)
- Input: DWORD port number
- Output: BYTE value

This requires knowing the specific chip model and register maps — varies by motherboard.

### Embedded Controller (EC)

Laptops expose battery, temperature, and fan data through the EC (Embedded Controller) at IO ports 0x62/0x66.

### ACPI Thermal Zones

`MSAcpi_ThermalZoneTemperature` via WMI — but the user wants to avoid WMI. Could read ACPI tables directly from physical memory via WinRing0, but very involved.

---

## Recommended Next Step

**CPU Core Voltage (Intel)** — adds `IOCTL_OLS_READ_MSR` with MSR 0x198, just like temperature. Same code path, new register address. Minimal effort, useful data.

**CPU Package Power (Intel RAPL)** — adds MSR 0x610 + 0x606, same DeviceIoControl path. Requires a time delta calculation between readings.

Both Intel sensors use the same driver infrastructure we already have — just new register addresses and parsing.
