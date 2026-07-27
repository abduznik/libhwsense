# libhwsense

Open-source, cross-vendor hardware sensor monitoring library in C.

A permanently free, MIT-licensed alternative to closed-source tools like HWInfo.

## What This Is

A lightweight C library that reads CPU temperature, voltage, and power directly from hardware registers using the WinRing0 kernel driver — no WMI, no bloated frameworks, no telemetry.

## Supported Hardware

| Vendor | Sensor | Method | Status |
|--------|--------|--------|--------|
| Intel | Per-core temperature | MSR 0x19C / 0x1A2 | Tested (i5-1235U) |
| AMD | Package temperature (Tctl) | SMN 0x00059800 via PCI config | Tested (Ryzen 5 4500) |
| AMD | CCD per-die temperatures | SMN 0x00059954+ | Tested (unavailable on Renoir) |
| AMD | Core voltage (VDDCR_CPU) | SVI2 Plane0 (SMN 0x0005A010) | Implemented |
| AMD | SoC voltage (VDDCR_SOC) | SVI2 Plane1 (SMN 0x0005A00C) | Implemented |
| AMD | Package power | SVI2 V*I telemetry | Implemented |

## Building

```powershell
# Configure
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# Build static lib + example
cmake --build build --config Release

# Build DLL
cmake --build build --config Release --target hwsense_shared
```

Requires:
- Windows (WinRing0 kernel driver)
- CMake 3.15+
- MSVC (Visual Studio 2022 or Build Tools)
- Administrator privileges (for kernel driver)

## Quick Start

```c
#include "hwsense.h"

hwsense_ctx_t *ctx = hwsense_init();
if (!ctx) return 1;

hwsense_temp_result_t temp = hwsense_cpu_package_temp(ctx);
if (temp.ok)
    printf("Temperature: %.1f C\n", temp.celsius);

hwsense_shutdown(ctx);
```

## How It Works

```
User mode (C library)
    |
    |  DeviceIoControl(handle, IOCTL_xxx, input, output)
    |
    v
WinRing0 kernel driver (WinRing0x64.sys)
    |
    |  Executes privileged instructions (RDMSR, PCI config R/W)
    |  on the calling thread's current CPU core
    |
    v
Hardware (MSRs, PCI config space, SMN bus)
```

The library installs WinRing0 as a kernel service via the Windows SCM, communicates via `DeviceIoControl`, and tears down on shutdown.

## Extending to Other Operating Systems

See `src/common/genericOS.c` — documented stubs for:
- MSR reads (`os_read_msr`)
- PCI config reads/writes (`os_read_pci_config`, `os_write_pci_config`)
- Thread affinity (`os_pin_thread`, `os_restore_thread`)
- Sleep (`os_sleep_ms`)

To port: implement these functions for your OS, add your `.c` to the build.

## WinRing0 Driver

The library requires `WinRing0x64.sys`, which must be placed next to the executable.

Where to get it:
- **OpenHardwareMonitor repo:** https://github.com/openhardwaremonitor/openhardwaremonitor (file: `Hardware/WinRing0x64.sys`)
- **LibreHardwareMonitor v0.9.3 or earlier:** https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases

Windows Defender will flag this as `HackTool:Win32/WinRing0`. This is expected — it's a signed kernel driver. Add an exclusion if needed.

## Project Structure

```
libhwsense/
├── include/
│   └── hwsense.h              Public API
├── src/
│   ├── common/
│   │   ├── ioctl_codes.h      IOCTL constants, register addresses
│   │   ├── hwsense_internal.h Internal context struct
│   │   └── genericOS.c        OS abstraction stubs
│   ├── driver/
│   │   └── driver.c           SCM lifecycle, vendor dispatch
│   ├── intel/
│   │   └── intel.c            MSR-based per-core temperature
│   └── amd/
│       └── amd_zen.c          SMN/SVI2/SMU mailbox
├── examples/
│   └── read_cpu_temp.c        CLI tool
├── .github/workflows/
│   └── build.yml              CI stub
├── CMakeLists.txt
├── LICENSE
├── README.md
└── .gitignore
```

## License

MIT — see [LICENSE](LICENSE).

## Contributing

1. Fork, branch, PR
2. Follow the existing code style (C89-ish, Win32 conventions)
3. Test on real hardware before submitting sensor changes
4. Register addresses must be cited (Intel SDM, AMD PPR, etc.)

## Why This Exists

Hardware monitoring tools like HWInfo are closed-source and can be acquired or enshittified. This library is MIT-licensed — it can never be locked down, paywalled, or taken private. The code is simple enough to audit and fork.
