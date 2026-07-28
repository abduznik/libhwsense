# libhwsense — CPU Temperature via WinRing0

Read CPU temperature directly from hardware using Model-Specific Registers (Intel) or SMN via PCI config space (AMD), bypassing WMI entirely.

## What We Built

A C library (`hwsense.lib`) and CLI tool (`read_cpu_temp.exe`) that read CPU temperature by communicating directly with the WinRing0 kernel driver via `DeviceIoControl`. Also includes a Python prototype (`cpu_temp_msr.py`) that does the same thing.

**Supported CPUs:**
- **Intel:** Per-core temperature via MSR 0x19C (IA32_THERM_STATUS) + MSR 0x1A2 (IA32_TEMPERATURE_TARGET)
- **AMD:** Package temperature (Tctl) via SMN register 0x00059800 through PCI config space (Zen/Zen2/Zen3/Zen4)
- **AMD:** Core voltage (VDDCR_CPU) via SVI2 Plane0 (SMN 0x0005A010)
- **AMD:** SoC voltage (VDDCR_SOC) via SVI2 Plane1 (SMN 0x0005A00C)

**Tested on:** AMD Ryzen 5 4500 (Zen 2 / Renoir, Family 17h Model 60h) — reports 46.8°C Tctl

---

## How WinRing0 Works

WinRing0 (`WinRing0x64.sys`) is a signed kernel driver that gives user-mode code ring-0 access. It's used by OpenHardwareMonitor, LibreHardwareMonitor (pre-v0.9.4), and similar tools.

### Communication Pattern

```
User mode (Python/C)
    │
    │  CreateFile("\\.\WinRing0_1_2_0")  →  get device handle
    │
    │  DeviceIoControl(handle, IOCTL_xxx, input, output)
    │
    ▼
WinRing0 kernel driver (.sys)
    │
    │  Executes privileged instructions (RDMSR, PCI config R/W)
    │  on the calling thread's current CPU core
    │
    ▼
Hardware (MSRs, PCI config space, IO ports)
```

### SCM Lifecycle

```
1. OpenSCManager()           → get SCM handle
2. CreateService()           → register driver as a service
3. StartService()            → load .sys into kernel memory
4. CreateFile("\\.\WinRing0_1_2_0") → open device handle
5. ... do work ...
6. CloseHandle()             → close device
7. ControlService(STOP)      → unload driver from kernel
8. DeleteService()           → remove service registration
9. CloseServiceHandle()      → release SCM handles
```

### Finding the .sys File

The driver file is searched in this order:
1. Same directory as the executable/script
2. Current working directory

Where to get it:
- **OpenHardwareMonitor repo:** https://github.com/openhardwaremonitor/openhardwaremonitor (file: `Hardware/WinRing0x64.sys`)
- **LibreHardwareMonitor releases v0.9.3 or earlier:** https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases (v0.9.4+ switched to PawnIO driver)

**WARNING:** Windows Defender / antivirus will flag `WinRing0x64.sys` as `HackTool:Win32/WinRing0`. This is expected — it's a signed kernel driver that gives ring-0 memory access. Add an exclusion in Windows Security if needed.

---

## IOCTL Codes

All IOCTL codes are derived from OpenHardwareMonitor's `IOControlCode.cs`:

```
CTL_CODE = (device_type << 16) | (access << 14) | (function << 2) | method

device_type = 40000 (0x9C40)
method      = METHOD_BUFFERED (0)
```

| IOCTL | Function | Access | Code | Input | Output |
|-------|----------|--------|------|-------|--------|
| `IOCTL_OLS_READ_MSR` | 0x821 | Any (0) | `0x9C402084` | `DWORD msr_index` | `DWORD64 msr_value` |
| `IOCTL_OLS_READ_PCI_CONFIG` | 0x851 | Read (1) | `0x9C406144` | `READ_PCI_CONFIG_INPUT` | `DWORD value` |
| `IOCTL_OLS_WRITE_PCI_CONFIG` | 0x852 | Write (2) | `0x9C40A148` | `WRITE_PCI_CONFIG_INPUT` | none |

**Critical detail:** The `Access` field goes into bits 15:14. Getting this wrong causes `ERROR_INVALID_FUNCTION` (Win32 error 1) — the driver doesn't recognize the IOCTL code. The MSR read uses `Access.Any = 0`, while PCI config uses `Access.Read = 1` and `Access.Write = 2`.

### PCI Config Structures

```c
typedef struct {
    DWORD PciAddress;   /* (bus << 8) | (device << 3) | function */
    DWORD RegAddress;   /* PCI config register offset */
} READ_PCI_CONFIG_INPUT;  /* 8 bytes, packed */

typedef struct {
    DWORD PciAddress;
    DWORD RegAddress;
    DWORD Value;
} WRITE_PCI_CONFIG_INPUT; /* 12 bytes, packed */
```

---

## Intel Temperature (MSR-based)

### Registers

| MSR | Name | Purpose |
|-----|------|---------|
| `0x1A2` | IA32_TEMPERATURE_TARGET | TjMax (max junction temp) |
| `0x19C` | IA32_THERM_STATUS | Current thermal status |

### Bit Layout

**MSR 0x1A2 (IA32_TEMPERATURE_TARGET):**
- Bits [23:16] — TjMax in °C (typically 100)

**MSR 0x19C (IA32_THERM_STATUS):**
- Bit 31 — Reading valid (1 = sensor is reporting)
- Bits [22:16] — Digital readout (distance from TjMax in °C)

### Formula

```
temperature = TjMax - digital_readout
```

### Thread Affinity

RDMSR executes on the **current logical processor**. To read a specific core's MSR, you must pin the calling thread to that core first:

```c
HANDLE thread = GetCurrentThread();
DWORD_PTR old_mask = SetThreadAffinityMask(thread, 1 << core_id);
// ... RDMSR ...
SetThreadAffinityMask(thread, old_mask);  // ALWAYS restore
```

This is the "RdmsrTx" pattern from OpenHardwareMonitor. Without it, the OS scheduler can migrate your thread between cores, giving you inconsistent readings.

---

## AMD Temperature (SMN via PCI Config)

AMD CPUs don't expose temperature via standard MSRs. Instead, they use the **SMN (System Management Network)** bus, which is accessed through PCI config space on the Root Complex (Bus 0, Device 0, Function 0).

### SMN Access Protocol

```
1. PCI config WRITE: Bus0/Dev0/Func0, offset 0x60 = SMN address
2. PCI config READ:  Bus0/Dev0/Func0, offset 0x64 = data
```

### Temperature Register

**SMN address:** `0x00059800` (F17H_M01H_THM_TCON_CUR_TMP)

**Bit layout:**
- Bits [31:21] — CUR_TEMP (current temperature in 0.125°C units)
- Bit [19] — RANGE_SEL (if set, apply -49°C offset)
- Bits [17:16] — TJ_SEL (if == 0b11, also signals -49°C offset)

### Formula (from LibreHardwareMonitor)

```c
temp = ((raw >> 21) * 125) / 1000.0;
if ((raw & 0x80000) || ((raw & 0x30000) == 0x30000))
    temp -= 49.0;
```

### Per-Core vs Package

AMD reports a **single Tctl (package temperature)** for the entire CPU, not per-core temps like Intel. All cores share the same sensor reading.

### PCI Address Packing

```c
DWORD pci_address = ((bus & 0xFF) << 8) | ((device & 0x1F) << 3) | (function & 7);
// Bus 0, Dev 0, Func 0 = 0x00000000
```

---

## CPU Vendor Detection

Read from the Windows registry:

```
HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\CentralProcessor\0
  VendorIdentifier = "AuthenticAMD" or "GenuineIntel"
```

---

## Troubleshooting

### Error 31 (ERROR_GEN_FAILURE) on MSR reads

The driver recognized the IOCTL but RDMSR faulted with a General Protection fault. This means the MSR doesn't exist on your CPU. Most likely cause: **you have an AMD CPU** and tried to read Intel MSRs (0x19C, 0x1A2). Use the SMN path instead.

### Error 1 (ERROR_INVALID_FUNCTION) on PCI config

The driver doesn't recognize the IOCTL code. Check that the `Access` bits are correct:
- `IOCTL_OLS_READ_PCI_CONFIG` must use `FILE_READ_ACCESS` (1) → `0x9C406144`
- `IOCTL_OLS_WRITE_PCI_CONFIG` must use `FILE_WRITE_ACCESS` (2) → `0x9C40A148`

Using `FILE_ANY_ACCESS` (0) gives the wrong IOCTL code and the driver returns "invalid function."

### ERROR_SERVICE_MARKED_FOR_DELETE (1072)

The previous run's cleanup called `DeleteService()` but the SCM hasn't finished removing it. The fix: open the stale service handle, stop it, close the handle (this forces the SCM to complete deletion), then retry `CreateServiceW`.

### Service won't start / driver won't load

- Check you're running as Administrator
- Check Windows Defender isn't blocking the .sys file (add exclusion)
- Check the .sys file is the 64-bit version (`WinRing0x64.sys`, not `WinRing0.sys`)

---

## Building

```powershell
# Configure
cmake -S F:\Coding\sensors-test\libhwsense -B F:\Coding\sensors-test\libhwsense\build -G "Visual Studio 17 2022" -A x64

# Build
cmake --build F:\Coding\sensors-test\libhwsense\build --config Release

# Run (as Administrator)
F:\Coding\sensors-test\libhwsense\build\Release\read_cpu_temp.exe
```

---

## Project Structure

```
libhwsense/
├── include/
│   └── hwsense.h              Public API (hwsense_init, shutdown, cpu_package_temp, cpu_core_voltage)
├── src/
│   ├── core/
│   │   ├── ioctl_codes.h      IOCTL constants, PCI structs, MSR/SMN/SVI2 defines
│   │   └── driver.c           SCM lifecycle, DeviceIoControl wrapper, vendor dispatch
│   └── cpu/
│       ├── amd_zen.c          SMN read via PCI config → Tctl formula, SVI2 voltage
│       └── intel.c            MSR 0x19C/0x1A2 per-core temp with thread affinity
├── examples/
│   └── read_cpu_temp.c        CLI: admin check → init → read → print → shutdown
├── CMakeLists.txt
└── build/Release/
    ├── read_cpu_temp.exe
    ├── hwsense.lib
    └── WinRing0x64.sys         (must be placed here or next to the exe)

cpu_temp_msr.py                 Python prototype (same logic, ctypes-based)
```

---

## Key Lessons Learned

1. **ctypes restype/argtypes are mandatory on x86_64.** Without declaring `restype = HANDLE` (64-bit), ctypes assumes `c_int` (32-bit) and truncates 64-bit handles. Every subsequent call fails with `ERROR_INVALID_HANDLE` (6).

2. **IOCTL Access bits matter.** The `Access` field in `CTL_CODE` goes into bits 15:14. MSR reads use `FILE_ANY_ACCESS` (0), but PCI config reads use `FILE_READ_ACCESS` (1) and writes use `FILE_WRITE_ACCESS` (2). Getting this wrong causes `ERROR_INVALID_FUNCTION` (1).

3. **RDMSR faults on the wrong CPU.** MSR 0x19C and 0x1A2 are Intel-only. Reading them on AMD causes #GP (General Protection fault). The driver catches the fault and returns `ERROR_GEN_FAILURE` (31).

4. **Thread affinity is required for per-core MSR reads.** RDMSR executes on the current logical processor. You must pin the thread to the target core before reading, and always restore affinity afterward.

5. **SCM service cleanup is racy.** `DeleteService()` marks a service for deletion but doesn't remove it immediately. The SCM waits for all handles to close. Force-close stale handles to speed up the process.

6. **AMD uses PCI config, not MSRs.** AMD Zen CPUs expose temperature through the SMN bus, accessed via PCI config space on Bus 0/Dev 0/Func 0 (offsets 0x60 and 0x64).
