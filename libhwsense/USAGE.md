# Usage Guide

This guide shows how to use libhwsense from Python, C#, and Rust/Tauri to build your own hardware monitoring applications.

## Quick Start

### Files Needed

```
your-app/
├── hwsense.dll          ← Main library (from releases)
├── WinRing0x64.sys      ← Kernel driver (auto-loaded by DLL)
└── LICENSE              ← BSD 2-Clause for WinRing0
```

### Requirements

- **Windows 10/11** (64-bit)
- **Administrator privileges** (for driver loading)
- **.NET Framework 4.5+** (for C#) or **Python 3.7+** (for Python)

---

## Python Examples

### Installation

No pip package needed — just use `ctypes` (built into Python):

```python
import ctypes
import os

# Load the DLL from same directory as script
dll_path = os.path.join(os.path.dirname(__file__), "hwsense.dll")
hwsense = ctypes.CDLL(dll_path)
```

### Basic Temperature Reading

```python
import ctypes
import os

# Load DLL
dll_path = os.path.join(os.path.dirname(__file__), "hwsense.dll")
hwsense = ctypes.CDLL(dll_path)

# Define struct for temperature result
class TempResult(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_int),
        ("celsius", ctypes.c_double),
        ("error", ctypes.c_char * 256)
    ]

# Initialize (loads WinRing0 driver)
ctx = hwsense.hwsense_init()
if not ctx:
    print("Failed to initialize - run as Administrator!")
    exit(1)

# Read CPU temperature
temp = hwsense.hwsense_cpu_package_temp(ctx)
if temp.ok:
    print(f"CPU Temperature: {temp.celsius:.1f}°C")
else:
    print(f"Error: {temp.error.decode()}")

# Read GPU info
class GpuInfo(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_int),
        ("temperature", ctypes.c_int),
        ("vram_used_mb", ctypes.c_int),
        ("vram_total_mb", ctypes.c_int),
        ("power_usage_w", ctypes.c_int),
        ("power_limit_w", ctypes.c_int),
        ("gpu_load", ctypes.c_int),
        ("mem_load", ctypes.c_int),
        ("clock_mhz", ctypes.c_int),
        ("mem_clock_mhz", ctypes.c_int),
        ("name", ctypes.c_char * 128),
        ("error", ctypes.c_char * 256)
    ]

gpu = hwsense.hwsense_gpu_info(0)
if gpu.ok:
    print(f"GPU: {gpu.name.decode()}")
    print(f"Temperature: {gpu.temperature}°C")
    print(f"VRAM: {gpu.vram_used_mb}/{gpu.vram_total_mb} MB")
    print(f"Power: {gpu.power_usage_w} W")

# Cleanup
hwsense.hwsense_shutdown(ctx)
```

### Monitoring Loop

```python
import ctypes
import time

# ... load DLL and structs as above ...

ctx = hwsense.hwsense_init()
if not ctx:
    print("Failed to initialize!")
    exit(1)

print("Monitoring... Press Ctrl+C to stop\n")

try:
    while True:
        # Read CPU
        temp = hwsense.hwsense_cpu_package_temp(ctx)
        if temp.ok:
            print(f"CPU: {temp.celsius:.1f}°C", end="  ")
        
        # Read GPU
        gpu = hwsense.hwsense_gpu_info(0)
        if gpu.ok:
            print(f"GPU: {gpu.temperature}°C ({gpu.vram_used_mb}/{gpu.vram_total_mb} MB)",
                  end="  ")
        
        # Read system
        class CpuLoad(ctypes.Structure):
            _fields_ = [("percent", ctypes.c_double), ("num_cpus", ctypes.c_int)]
        
        load = CpuLoad()
        if hwsense.win_get_cpu_load_sampled(ctypes.byref(load)):
            print(f"Load: {load.percent:.1f}%")
        else:
            print()
        
        time.sleep(2)
except KeyboardInterrupt:
    print("\nStopped.")

hwsense.hwsense_shutdown(ctx)
```

---

## C# Examples

### P/Invoke Declarations

```csharp
using System;
using System.Runtime.InteropServices;

public static class Hwsense
{
    private const string DllName = "hwsense.dll";

    [StructLayout(LayoutKind.Sequential)]
    public struct TempResult
    {
        public int ok;
        public double celsius;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string error;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct GpuInfo
    {
        public int ok;
        public int temperature;
        public int vram_used_mb;
        public int vram_total_mb;
        public int power_usage_w;
        public int power_limit_w;
        public int gpu_load;
        public int mem_load;
        public int clock_mhz;
        public int mem_clock_mhz;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string error;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CpuLoad
    {
        public double percent;
        public int num_cpus;
    }

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr hwsense_init();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hwsense_shutdown(IntPtr ctx);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern TempResult hwsense_cpu_package_temp(IntPtr ctx);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern GpuInfo hwsense_gpu_info(int gpuIndex);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    public static extern int win_get_cpu_load_sampled(ref CpuLoad load);
}
```

### Usage Example

```csharp
using System;

class Program
{
    static void Main()
    {
        // Initialize
        IntPtr ctx = Hwsense.hwsense_init();
        if (ctx == IntPtr.Zero)
        {
            Console.WriteLine("Failed to initialize - run as Administrator!");
            return;
        }

        Console.WriteLine("Hardware Sensor Report\n");

        // CPU Temperature
        var temp = Hwsense.hwsense_cpu_package_temp(ctx);
        if (temp.ok == 1)
            Console.WriteLine($"CPU Temperature: {temp.celsius:F1}°C");
        else
            Console.WriteLine($"CPU Temperature: {temp.error}");

        // GPU Info
        var gpu = Hwsense.hwsense_gpu_info(0);
        if (gpu.ok == 1)
        {
            Console.WriteLine($"\nGPU: {gpu.name}");
            Console.WriteLine($"Temperature: {gpu.temperature}°C");
            Console.WriteLine($"VRAM: {gpu.vram_used_mb}/{gpu.vram_total_mb} MB");
            Console.WriteLine($"Power: {gpu.power_usage_w} W");
            Console.WriteLine($"Clock: {gpu.clock_mhz} MHz");
        }

        // CPU Load
        var load = new Hwsense.CpuLoad();
        if (Hwsense.win_get_cpu_load_sampled(ref load) == 1)
            Console.WriteLine($"\nCPU Load: {load.percent:F1}% ({load.num_cpus} cores)");

        // Cleanup
        Hwsense.hwsense_shutdown(ctx);
    }
}
```

---

## Rust / Tauri Examples

### FFI Bindings

```rust
// src/hwsense.rs

use std::ffi::{c_int, c_double, CStr};
use std::os::raw::c_char;

#[repr(C)]
pub struct TempResult {
    pub ok: c_int,
    pub celsius: c_double,
    pub error: [c_char; 256],
}

#[repr(C)]
pub struct GpuInfo {
    pub ok: c_int,
    pub temperature: c_int,
    pub vram_used_mb: c_int,
    pub vram_total_mb: c_int,
    pub power_usage_w: c_int,
    pub power_limit_w: c_int,
    pub gpu_load: c_int,
    pub mem_load: c_int,
    pub clock_mhz: c_int,
    pub mem_clock_mhz: c_int,
    pub name: [c_char; 128],
    pub error: [c_char; 256],
}

#[repr(C)]
pub struct CpuLoad {
    pub percent: c_double,
    pub num_cpus: c_int,
}

extern "C" {
    pub fn hwsense_init() -> *mut std::ffi::c_void;
    pub fn hwsense_shutdown(ctx: *mut std::ffi::c_void);
    pub fn hwsense_cpu_package_temp(ctx: *mut std::ffi::c_void) -> TempResult;
    pub fn hwsense_gpu_info(gpu_index: c_int) -> GpuInfo;
    pub fn win_get_cpu_load_sampled(load: *mut CpuLoad) -> c_int;
}

pub fn safe_temp(ctx: *mut std::ffi::c_void) -> Option<f64> {
    let result = unsafe { hwsense_cpu_package_temp(ctx) };
    if result.ok == 1 {
        Some(result.celsius)
    } else {
        None
    }
}
```

### Tauri Integration

```rust
// src-tauri/src/main.rs

mod hwsense;

use tauri::command;

#[command]
fn get_cpu_temp() -> Result<f64, String> {
    unsafe {
        let ctx = hwsense::hwsense_init();
        if ctx.is_null() {
            return Err("Failed to initialize hardware sensors".into());
        }

        let temp = hwsense::hwsense_cpu_package_temp(ctx);
        hwsense::hwsense_shutdown(ctx);

        if temp.ok == 1 {
            Ok(temp.celsius)
        } else {
            Err("Failed to read temperature".into())
        }
    }
}

#[command]
fn get_gpu_info() -> Result<hwsense::GpuInfo, String> {
    let info = unsafe { hwsense::hwsense_gpu_info(0) };
    if info.ok == 1 {
        Ok(info)
    } else {
        Err("No GPU found".into())
    }
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![get_cpu_temp, get_gpu_info])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
```

### Frontend (JavaScript)

```javascript
// In your Tauri app
import { invoke } from '@tauri-apps/api/tauri';

async function updateSensors() {
    try {
        const temp = await invoke('get_cpu_temp');
        document.getElementById('cpu-temp').textContent = `${temp.toFixed(1)}°C`;
    } catch (error) {
        console.error('Failed to read CPU temp:', error);
    }

    try {
        const gpu = await invoke('get_gpu_info');
        document.getElementById('gpu-info').textContent = 
            `${gpu.name}: ${gpu.temperature}°C (${gpu.vram_used_mb}/${gpu.vram_total_mb} MB)`;
    } catch (error) {
        console.error('Failed to read GPU info:', error);
    }
}

// Update every 2 seconds
setInterval(updateSensors, 2000);
```

---

## Available Functions Reference

### Core Functions

| Function | Description | Returns |
|----------|-------------|---------|
| `hwsense_init()` | Initialize library and load driver | Context handle (NULL on failure) |
| `hwsense_shutdown(ctx)` | Cleanup and unload driver | void |
| `hwsense_cpu_package_temp(ctx)` | Read CPU temperature | TempResult struct |
| `hwsense_cpu_core_clock(ctx)` | Read CPU clock (MHz) | int (-1 on failure) |
| `hwsense_cpu_core_voltage_value(ctx)` | Read CPU voltage (V) | double (-1.0 on failure) |
| `hwsense_cpu_package_power_watts(ctx)` | Read CPU power (W) | double (-1.0 on failure) |

### GPU Functions

| Function | Description | Returns |
|----------|-------------|---------|
| `hwsense_gpu_info(gpu_index)` | Read GPU temperature, VRAM, power, load | GpuInfo struct |

### System Functions

| Function | Description | Returns |
|----------|-------------|---------|
| `win_get_cpu_load_sampled(load)` | Read CPU load (1s sampling) | int (1=success) |
| `win_get_mem_stats(mem)` | Read memory usage | int (1=success) |
| `win_get_disk_stats(disks, max)` | Read disk usage | int (count) |
| `win_get_uptime()` | Read system uptime | Uptime struct |

### Diagnostic Functions

| Function | Description | Returns |
|----------|-------------|---------|
| `cpu_diag_detect()` | Detect CPU features | CpuDiagResult struct |
| `cpu_diag_print(diag)` | Print diagnostic report | void |

---

## Error Handling

All functions return structured results with an `ok` field:

```python
temp = hwsense.hwsense_cpu_package_temp(ctx)
if temp.ok:
    print(f"Success: {temp.celsius}°C")
else:
    print(f"Error: {temp.error.decode()}")
```

Common errors:
- **"Invalid context or driver handle"** — Call `hwsense_init()` first
- **"RDMSR failed"** — CPU doesn't support this MSR (check vendor)
- **"SMN read failed"** — AMD SMN access failed (check PCI config)
- **"No GPU temperature available"** — NVML/ADL not installed

---

## Performance Notes

- **CPU Load**: Uses 1-second sampling interval for accurate readings
- **GPU Info**: Direct NVML calls, minimal overhead
- **Driver Loading**: First call to `hwsense_init()` loads the kernel driver (~100ms)
- **Thread Safety**: Each context is single-threaded; use separate contexts for multi-threaded access

---

## Troubleshooting

### "Run as Administrator" Error

The WinRing0 driver requires admin privileges. Right-click your app → "Run as administrator".

### Antivirus Blocking

Windows Defender may flag `WinRing0x64.sys`. Add an exclusion:
1. Open Windows Security → Virus & threat protection
2. Manage settings → Exclusions → Add an exclusion
3. Add the folder containing the .sys file

### DLL Not Found

Ensure `hwsense.dll` and `WinRing0x64.sys` are in the same directory as your executable.

### Temperature Shows 0 or N/A

- Check if your CPU is supported (run `cpu_diag_detect()`)
- Verify driver loaded successfully (check stderr output)
- Some AMD APUs don't expose voltage/power telemetry

---

## License

libhwsense is licensed under MIT. The included WinRing0 driver is licensed under BSD 2-Clause.

See [LICENSE](../LICENSE) for details.
