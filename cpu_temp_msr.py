"""
cpu_temp_msr.py — Read CPU temperature via WinRing0 driver.

Supports:
  - Intel: MSR 0x19C (IA32_THERM_STATUS) + MSR 0x1A2 (IA32_TEMPERATURE_TARGET)
  - AMD:   SMN register 0x00059800 via PCI config space (Zen/Zen2/Zen3/Zen4)

REQUIRES:
  - Windows, Python 3.7+ (ctypes only, no pip packages)
  - Run as Administrator (kernel driver can't load without elevation)
  - WinRing0x64.sys placed next to this script or in CWD

WINRING0 DRIVER:
  This script talks DIRECTLY to the WinRing0 kernel driver (.sys) via
  DeviceIoControl — no DLL wrapper needed.  Same approach as OpenHardwareMonitor.

  Where to get WinRing0x64.sys:
    - OpenHardwareMonitor repo:
      https://github.com/openhardwaremonitor/openhardwaremonitor
      (file: Hardware/WinRing0x64.sys — checked into the repo)
    - LibreHardwareMonitor releases v0.9.3 or EARLIER:
      https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases
      (v0.9.4+ switched to PawnIO driver — no WinRing0 .sys in the bundle)

  WARNING: Windows Defender / antivirus WILL likely flag WinRing0x64.sys as
  "HackTool:Win32/WinRing0".  This is expected — it's a signed kernel driver
  that gives ring-0 memory access.  Add an exclusion in Windows Security if
  needed.

HOW IT WORKS:
  Intel path:
    1. Pin thread to core → RDMSR 0x1A2 → TjMax
    2. Pin thread to core → RDMSR 0x19C → digital readout (bits 22:16)
    3. Temperature = TjMax - digital_readout

  AMD path (Zen/Zen2/Zen3/Zen4):
    1. PCI config write SMN address (0x00059800) to Bus0/Dev0/Func0 offset 0x60
    2. PCI config read data from Bus0/Dev0/Func0 offset 0x64
    3. Extract bits [31:21], multiply by 0.125, subtract 49 if offset flag set
    4. This gives Tctl (temperature control) — same for all cores (package temp)
"""

import ctypes
import ctypes.wintypes as wintypes
import os
import sys
import time

# ---------------------------------------------------------------------------
# CONSTANTS
# ---------------------------------------------------------------------------

# Intel MSR addresses (Intel SDM Vol. 4)
MSR_IA32_THERM_STATUS       = 0x19C
MSR_IA32_TEMPERATURE_TARGET = 0x1A2

# AMD SMN (System Management Network) temperature register
# Accessed via PCI config space on Bus 0, Device 0, Function 0
AMD_F17H_TEMP_REGISTER      = 0x00059800
SMN_INDEX_OFFSET            = 0x60   # PCI config offset to write SMN address
SMN_DATA_OFFSET             = 0x64   # PCI config offset to read SMN data

# WinRing0 driver
WINRING0_DEVICE      = r"\\.\WinRing0_1_2_0"
WINRING0_SERVICE_NAME = "WinRing0_1_2_0"
WINRING0_SYS_FILENAME = "WinRing0x64.sys"

# IOCTL codes — derived from OpenHardwareMonitor's IOControlCode constructor:
#   CTL_CODE = (device_type << 16) | (access << 14) | (function << 2) | method
#   device_type = 40000 (0x9C40)
#
#   Access values (from OpenHardwareMonitor IOControlCode.Access enum):
#     Any  = 0  (FILE_ANY_ACCESS)
#     Read = 1  (FILE_READ_ACCESS)  — used for PCI config read
#     Write= 2  (FILE_WRITE_ACCESS) — used for PCI config write
#
IOCTL_OLS_READ_MSR         = 0x9C402084  # func=0x821, access=Any(0),   method=BUFFERED
IOCTL_OLS_READ_PCI_CONFIG  = 0x9C406144  # func=0x851, access=Read(1),  method=BUFFERED
IOCTL_OLS_WRITE_PCI_CONFIG = 0x9C40A148  # func=0x852, access=Write(2), method=BUFFERED

# Windows constants
GENERIC_READ          = 0x80000000
GENERIC_WRITE         = 0x40000000
OPEN_EXISTING         = 3
FILE_ATTRIBUTE_NORMAL = 0x80
INVALID_HANDLE_VALUE  = ctypes.c_void_p(-1).value

# SCM constants
SC_MANAGER_ALL_ACCESS = 0xF003F
SERVICE_ALL_ACCESS    = 0xF01FF
SERVICE_KERNEL_DRIVER = 1
SERVICE_DEMAND_START  = 3
SERVICE_ERROR_NORMAL  = 1
SERVICE_CONTROL_STOP  = 1

# PCI config input structures — must match WinRing0 driver's expected layout.
# OpenHardwareMonitor uses these exact struct layouts:
#   ReadPciConfigInput  { uint PciAddress; uint RegAddress; }  — 8 bytes
#   WritePciConfigInput { uint PciAddress; uint RegAddress; uint Value; } — 12 bytes

class ReadPciConfigInput(ctypes.Structure):
    _fields_ = [
        ("PciAddress", wintypes.DWORD),
        ("RegAddress", wintypes.DWORD),
    ]

class WritePciConfigInput(ctypes.Structure):
    _fields_ = [
        ("PciAddress", wintypes.DWORD),
        ("RegAddress", wintypes.DWORD),
        ("Value",      wintypes.DWORD),
    ]

# Load Win32 DLLs
kernel32 = ctypes.windll.kernel32
advapi32 = ctypes.windll.advapi32

# ---------------------------------------------------------------------------
# Win32 function prototypes (restype / argtypes)
#
# CRITICAL: Without these, ctypes assumes 32-bit int return values.  On
# 64-bit Windows, handle-returning functions get truncated to 32 bits.
# ---------------------------------------------------------------------------

kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
    wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
]

kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

kernel32.DeviceIoControl.restype = wintypes.BOOL
kernel32.DeviceIoControl.argtypes = [
    wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
    wintypes.LPVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
]

kernel32.SetThreadAffinityMask.restype = wintypes.DWORD
kernel32.SetThreadAffinityMask.argtypes = [wintypes.HANDLE, wintypes.DWORD]

kernel32.GetCurrentThread.restype = wintypes.HANDLE
kernel32.GetCurrentThread.argtypes = []

kernel32.GetSystemInfo.restype = None
kernel32.GetSystemInfo.argtypes = [wintypes.LPVOID]

advapi32.OpenSCManagerW.restype = wintypes.HANDLE
advapi32.OpenSCManagerW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD]

advapi32.CreateServiceW.restype = wintypes.HANDLE
advapi32.CreateServiceW.argtypes = [
    wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
    wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.LPCWSTR,
    wintypes.LPCWSTR, wintypes.LPDWORD, wintypes.LPCWSTR,
    wintypes.LPCWSTR, wintypes.LPCWSTR,
]

advapi32.OpenServiceW.restype = wintypes.HANDLE
advapi32.OpenServiceW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.DWORD]

advapi32.StartServiceW.restype = wintypes.BOOL
advapi32.StartServiceW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID]

advapi32.ControlService.restype = wintypes.BOOL
advapi32.ControlService.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID]

advapi32.DeleteService.restype = wintypes.BOOL
advapi32.DeleteService.argtypes = [wintypes.HANDLE]

advapi32.CloseServiceHandle.restype = wintypes.BOOL
advapi32.CloseServiceHandle.argtypes = [wintypes.HANDLE]


# ---------------------------------------------------------------------------
# CPU VENDOR DETECTION
# ---------------------------------------------------------------------------

def get_cpu_vendor():
    """Read CPU vendor string from the Windows registry."""
    import winreg
    try:
        key = winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"HARDWARE\DESCRIPTION\System\CentralProcessor\0"
        )
        vendor, _ = winreg.QueryValueEx(key, "VendorIdentifier")
        winreg.CloseKey(key)
        return vendor.strip()
    except Exception:
        return "Unknown"


def detect_cpu_vendor():
    """Return 'Intel' or 'AMD' based on the vendor string."""
    vendor = get_cpu_vendor()
    if "Intel" in vendor:
        return "Intel"
    elif "AMD" in vendor or "AuthenticAMD" in vendor:
        return "AMD"
    else:
        return vendor


# ---------------------------------------------------------------------------
# DRIVER MANAGEMENT
# ---------------------------------------------------------------------------

def is_admin():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def find_driver_file():
    search_dirs = [
        os.path.dirname(os.path.abspath(__file__)),
        os.getcwd(),
    ]
    for d in search_dirs:
        candidate = os.path.join(d, WINRING0_SYS_FILENAME)
        if os.path.isfile(candidate):
            return candidate
    return None


def open_scm():
    scm = advapi32.OpenSCManagerW(None, None, SC_MANAGER_ALL_ACCESS)
    if not scm:
        raise OSError(f"OpenSCManager failed. Win32 error: {ctypes.GetLastError()}")
    return scm


def install_and_start_driver(scm, driver_path, retries=3):
    ERROR_SERVICE_EXISTS            = 0x431
    ERROR_SERVICE_MARKED_FOR_DELETE = 0x440

    for attempt in range(retries):
        svc = advapi32.CreateServiceW(
            scm, WINRING0_SERVICE_NAME, WINRING0_SERVICE_NAME,
            SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
            driver_path, None, None, None, None, None,
        )
        if svc:
            break

        err = ctypes.GetLastError()
        if err == ERROR_SERVICE_EXISTS:
            svc = advapi32.OpenServiceW(scm, WINRING0_SERVICE_NAME, SERVICE_ALL_ACCESS)
            if not svc:
                raise OSError(f"OpenService failed. Win32 error: {ctypes.GetLastError()}")
            break
        elif err == ERROR_SERVICE_MARKED_FOR_DELETE:
            # Previous cleanup marked it for deletion but SCM hasn't finished.
            # Force the deletion forward: open the stale service, stop it,
            # close the handle (SCM waits for all handles to close), then wait.
            print(f"  [retry] Service marked for delete, forcing cleanup... ({attempt + 1}/{retries})")
            stale = advapi32.OpenServiceW(scm, WINRING0_SERVICE_NAME, SERVICE_ALL_ACCESS)
            if stale:
                advapi32.ControlService(stale, SERVICE_CONTROL_STOP, None)
                advapi32.CloseServiceHandle(stale)
            time.sleep(2)
            continue
        else:
            raise OSError(f"CreateService failed. Win32 error: {err}")
    else:
        raise OSError(f"Service stuck in MARKED_FOR_DELETE after {retries} retries. Reboot to clear.")

    ERROR_SERVICE_ALREADY_RUNNING = 0x420
    if not advapi32.StartServiceW(svc, 0, None):
        err = ctypes.GetLastError()
        if err != ERROR_SERVICE_ALREADY_RUNNING:
            raise OSError(f"StartService failed. Win32 error: {err}")

    return svc


def open_driver_handle():
    handle = kernel32.CreateFileW(
        WINRING0_DEVICE, GENERIC_READ | GENERIC_WRITE,
        0, None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None,
    )
    if handle == INVALID_HANDLE_VALUE:
        raise OSError(f"Failed to open WinRing0 device. Win32 error: {ctypes.GetLastError()}")
    return handle


def stop_and_remove_driver(scm, svc):
    if not svc:
        return
    advapi32.ControlService(svc, SERVICE_CONTROL_STOP, None)
    advapi32.DeleteService(svc)
    advapi32.CloseServiceHandle(svc)


# ---------------------------------------------------------------------------
# THREAD AFFINITY (needed for MSR reads — RDMSR executes on current core)
# ---------------------------------------------------------------------------

def pin_thread_to_core(core_id):
    """Pin current thread to a specific core.  Returns old affinity mask."""
    thread = kernel32.GetCurrentThread()
    mask = 1 << core_id
    old = kernel32.SetThreadAffinityMask(thread, mask)
    if not old:
        raise OSError(f"SetThreadAffinityMask(core {core_id}) failed. Win32 error: {ctypes.GetLastError()}")
    return thread, old


def restore_affinity(thread, old_mask):
    kernel32.SetThreadAffinityMask(thread, old_mask)


# ---------------------------------------------------------------------------
# LOW-LEVEL WinRing0 I/O
# ---------------------------------------------------------------------------

def read_msr(driver_handle, msr_index):
    """
    Read an MSR via WinRing0 IOCTL.  Returns (eax, edx).
    Must be called AFTER pinning thread to the target core.
    """
    input_val = ctypes.c_uint(msr_index)
    output_val = ctypes.c_uint64(0)
    bytes_ret = wintypes.DWORD(0)

    ok = kernel32.DeviceIoControl(
        driver_handle, IOCTL_OLS_READ_MSR,
        ctypes.byref(input_val), ctypes.sizeof(input_val),
        ctypes.byref(output_val), ctypes.sizeof(output_val),
        ctypes.byref(bytes_ret), None,
    )
    if not ok:
        raise OSError(f"RDMSR(0x{msr_index:X}) failed. Win32 error: {ctypes.GetLastError()}")

    raw = output_val.value
    return (raw & 0xFFFFFFFF, (raw >> 32) & 0xFFFFFFFF)


def get_pci_address(bus, device, function):
    """Pack bus/device/function into the uint32 format WinRing0 expects."""
    return ((bus & 0xFF) << 8) | ((device & 0x1F) << 3) | (function & 7)


def read_pci_config_dword(driver_handle, pci_address, reg_address):
    """Read a 32-bit value from PCI config space."""
    inp = ReadPciConfigInput(pci_address, reg_address)
    out_val = wintypes.DWORD(0)
    bytes_ret = wintypes.DWORD(0)

    ok = kernel32.DeviceIoControl(
        driver_handle, IOCTL_OLS_READ_PCI_CONFIG,
        ctypes.byref(inp), ctypes.sizeof(inp),
        ctypes.byref(out_val), ctypes.sizeof(out_val),
        ctypes.byref(bytes_ret), None,
    )
    if not ok:
        raise OSError(
            f"PCI config read failed (addr=0x{pci_address:X}, reg=0x{reg_address:X}). "
            f"Win32 error: {ctypes.GetLastError()}"
        )
    return out_val.value


def write_pci_config_dword(driver_handle, pci_address, reg_address, value):
    """Write a 32-bit value to PCI config space."""
    inp = WritePciConfigInput(pci_address, reg_address, value)

    ok = kernel32.DeviceIoControl(
        driver_handle, IOCTL_OLS_WRITE_PCI_CONFIG,
        ctypes.byref(inp), ctypes.sizeof(inp),
        None, 0, None, None,
    )
    if not ok:
        raise OSError(
            f"PCI config write failed (addr=0x{pci_address:X}, reg=0x{reg_address:X}). "
            f"Win32 error: {ctypes.GetLastError()}"
        )


def read_smn_register(driver_handle, smn_address):
    """
    Read a register via AMD's SMN (System Management Network).

    SMN is accessed through PCI config space on Bus 0 / Device 0 / Function 0
    (the Root Complex / Northbridge):
      1. Write the SMN address to config offset 0x60
      2. Read the data from config offset 0x64

    This is how LibreHardwareMonitor and ryzen_smu Linux driver do it.
    """
    nb = get_pci_address(0, 0, 0)  # Root Complex = Bus 0, Dev 0, Func 0
    write_pci_config_dword(driver_handle, nb, SMN_INDEX_OFFSET, smn_address)
    return read_pci_config_dword(driver_handle, nb, SMN_DATA_OFFSET)


# ---------------------------------------------------------------------------
# INTEL TEMPERATURE (MSR-based)
# ---------------------------------------------------------------------------

def get_intel_temperature(driver_handle, core_id):
    """
    Read Intel CPU core temperature via MSRs.

    Steps:
      1. RDMSR 0x1A2 → TjMax (bits 23:16)
      2. RDMSR 0x19C → digital readout (bits 22:16), bit 31 = valid
      3. Temperature = TjMax - digital_readout
    """
    thread, old_mask = pin_thread_to_core(core_id)
    try:
        # Read TjMax
        eax, _ = read_msr(driver_handle, MSR_IA32_TEMPERATURE_TARGET)
        tj_max = (eax >> 16) & 0xFF
        if tj_max < 50 or tj_max > 150:
            tj_max = 100  # sane fallback

        # Read thermal status
        eax, _ = read_msr(driver_handle, MSR_IA32_THERM_STATUS)
        reading_valid = bool(eax & (1 << 31))
        digital_readout = (eax >> 16) & 0x7F

        if not reading_valid:
            return {'temp': None, 'tj_max': tj_max, 'readout': digital_readout, 'error': 'reading not valid'}

        temp = tj_max - digital_readout
        return {'temp': float(temp), 'tj_max': tj_max, 'readout': digital_readout, 'error': None}

    except OSError as e:
        return {'temp': None, 'tj_max': None, 'readout': None, 'error': str(e)}
    finally:
        restore_affinity(thread, old_mask)


# ---------------------------------------------------------------------------
# AMD TEMPERATURE (SMN via PCI config)
# ---------------------------------------------------------------------------

def get_amd_temperature(driver_handle):
    """
    Read AMD CPU temperature via SMN register 0x00059800.

    This register is available on Zen (Family 17h) and later.
    It reports Tctl (temperature control) which is the same for all cores
    (package-level temperature).

    Bit layout of SMN 0x00059800:
      [31:21]  — CUR_TEMP (current temperature in units of 0.125°C)
      [19]     — RANGE_SEL (if set, apply -49°C offset)
      [17:16]  — TJ_SEL (if == 0b11, also signals -49°C offset)

    Temperature formula (from LibreHardwareMonitor):
      temp = (raw >> 21) * 0.125
      if RANGE_SEL or TJ_SEL == 0b11: temp -= 49.0

    Note: AMD reports a SINGLE temperature for the whole package (Tctl),
    not per-core temps like Intel.  All cores will show the same value.
    """
    try:
        raw = read_smn_register(driver_handle, AMD_F17H_TEMP_REGISTER)

        # Check offset flags
        range_sel = bool(raw & 0x80000)            # bit 19
        tj_sel    = (raw & 0x30000) == 0x30000     # bits [17:16] == 0b11
        offset_flag = range_sel or tj_sel

        # Extract temperature: bits [31:21], scaled by 0.125°C per unit
        temp = ((raw >> 21) * 125) / 1000.0
        if offset_flag:
            temp -= 49.0

        return {
            'temp': temp,
            'raw': raw,
            'offset_flag': offset_flag,
            'error': None,
        }
    except OSError as e:
        return {'temp': None, 'raw': None, 'offset_flag': None, 'error': str(e)}


# ---------------------------------------------------------------------------
# UTILITY
# ---------------------------------------------------------------------------

def get_logical_core_count():
    class SYSTEM_INFO(ctypes.Structure):
        _fields_ = [
            ("wProcessorArchitecture",      wintypes.WORD),
            ("wReserved",                   wintypes.WORD),
            ("dwPageSize",                  wintypes.DWORD),
            ("lpMinimumApplicationAddress", ctypes.c_void_p),
            ("lpMaximumApplicationAddress", ctypes.c_void_p),
            ("dwActiveProcessorMask",       ctypes.c_size_t),
            ("dwNumberOfProcessors",        wintypes.DWORD),
            ("dwProcessorType",             wintypes.DWORD),
            ("dwAllocationGranularity",     wintypes.DWORD),
            ("wProcessorLevel",             wintypes.WORD),
            ("wProcessorRevision",          wintypes.WORD),
        ]
    si = SYSTEM_INFO()
    kernel32.GetSystemInfo(ctypes.byref(si))
    return si.dwNumberOfProcessors


# ---------------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------------

def main():
    # --- Admin check ---
    if not is_admin():
        print("ERROR: Run as Administrator.")
        sys.exit(1)

    # --- Detect CPU vendor ---
    cpu_vendor = detect_cpu_vendor()
    num_cores = get_logical_core_count()

    print(f"CPU vendor:  {cpu_vendor}")
    print(f"Logical cores: {num_cores}")

    if cpu_vendor == "Intel":
        print("Method: Intel MSR (0x19C + 0x1A2)")
    elif cpu_vendor == "AMD":
        print("Method: AMD SMN (0x00059800 via PCI config)")
    else:
        print(f"WARNING: Unknown vendor '{cpu_vendor}' — will try both methods")

    # --- Find driver ---
    driver_path = find_driver_file()
    if driver_path is None:
        print(f"\nERROR: {WINRING0_SYS_FILENAME} not found.")
        print("Place it next to this script or in CWD.")
        print("\nWhere to get it:")
        print("  https://github.com/openhardwaremonitor/openhardwaremonitor")
        print("  (file: Hardware/WinRing0x64.sys)")
        print("\n  OR LibreHardwareMonitor releases v0.9.3 or earlier:")
        print("  https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases")
        sys.exit(1)

    print(f"Driver: {driver_path}")
    print()

    # --- Load driver ---
    scm_handle = None
    svc_handle = None
    driver_handle = None

    try:
        scm_handle = open_scm()
        svc_handle = install_and_start_driver(scm_handle, driver_path)
        driver_handle = open_driver_handle()
        print("WinRing0 driver loaded.\n")

        # --- Diagnostic test ---
        print("Diagnostic: testing sensor read on core 0...")

        if cpu_vendor == "Intel":
            try:
                thread, old_mask = pin_thread_to_core(0)
                try:
                    eax, edx = read_msr(driver_handle, MSR_IA32_TEMPERATURE_TARGET)
                    print(f"  MSR 0x1A2: EAX=0x{eax:08X}  TjMax={(eax >> 16) & 0xFF}C")
                finally:
                    restore_affinity(thread, old_mask)
            except OSError as e:
                print(f"  MSR 0x1A2 FAILED: {e}")

            try:
                thread, old_mask = pin_thread_to_core(0)
                try:
                    eax, edx = read_msr(driver_handle, MSR_IA32_THERM_STATUS)
                    print(f"  MSR 0x19C: EAX=0x{eax:08X}  valid={bool(eax & (1 << 31))}  readout={(eax >> 16) & 0x7F}")
                finally:
                    restore_affinity(thread, old_mask)
            except OSError as e:
                print(f"  MSR 0x19C FAILED: {e}")

        elif cpu_vendor == "AMD":
            # --- PCI config diagnostic ---
            nb = get_pci_address(0, 0, 0)
            print(f"  PCI address for Bus0/Dev0/Func0: 0x{nb:X}")
            print(f"  IOCTL_OLS_READ_PCI_CONFIG:  0x{IOCTL_OLS_READ_PCI_CONFIG:08X}")
            print(f"  IOCTL_OLS_WRITE_PCI_CONFIG: 0x{IOCTL_OLS_WRITE_PCI_CONFIG:08X}")
            print()

            # Test PCI config read first (vendor ID at offset 0)
            try:
                vendor_id = read_pci_config_dword(driver_handle, nb, 0x00)
                print(f"  PCI config read [reg=0x00]: 0x{vendor_id:08X} (vendor/device ID)")
            except OSError as e:
                print(f"  PCI config read FAILED: {e}")
                print()
                print("  The driver loaded but does not recognize PCI config IOCTLs.")
                print("  This WinRing0 .sys may be a stripped-down version without")
                print("  PCI support.  Try getting the .sys from OpenHardwareMonitor:")
                print("    https://github.com/openhardwaremonitor/openhardwaremonitor")

            # Test PCI config write + SMN read
            try:
                write_pci_config_dword(driver_handle, nb, SMN_INDEX_OFFSET, AMD_F17H_TEMP_REGISTER)
                print(f"  PCI config write [reg=0x60] = 0x{AMD_F17H_TEMP_REGISTER:08X}: OK")
                raw = read_pci_config_dword(driver_handle, nb, SMN_DATA_OFFSET)
                print(f"  PCI config read  [reg=0x64]: 0x{raw:08X}")

                range_sel = bool(raw & 0x80000)
                tj_sel = (raw & 0x30000) == 0x30000
                temp = ((raw >> 21) * 125) / 1000.0
                if range_sel or tj_sel:
                    temp -= 49.0
                print(f"  Parsed: Tctl = {temp:.1f} C")
            except OSError as e:
                print(f"  SMN read FAILED: {e}")

        print()
        input("Press Enter to start monitoring (Ctrl+C to quit)...\n")

        # --- Main loop ---
        try:
            while True:
                os.system('cls' if os.name == 'nt' else 'clear')

                if cpu_vendor == "Intel":
                    print(f"CPU Temperatures — Intel MSR (WinRing0)")
                    print(f"{'Core':<8} {'TjMax':<8} {'Readout':<10} {'Temp (C)':<10}")
                    print("-" * 38)
                    for core_id in range(num_cores):
                        r = get_intel_temperature(driver_handle, core_id)
                        if r['error']:
                            print(f"  {core_id:<6} {'---':<8} {'---':<10} ERR: {r['error'][:35]}")
                        elif r['temp'] is not None:
                            print(f"  {core_id:<6} {r['tj_max']:<8} {r['readout']:<10} {r['temp']:<10.1f}")
                        else:
                            print(f"  {core_id:<6} {r['tj_max']:<8} {r['readout']:<10} N/A")

                elif cpu_vendor == "AMD":
                    r = get_amd_temperature(driver_handle)
                    print(f"CPU Temperature — AMD SMN (WinRing0)")
                    print(f"  Tctl (package): ", end="")
                    if r['error']:
                        print(f"ERR: {r['error']}")
                    elif r['temp'] is not None:
                        print(f"{r['temp']:.1f} C  (raw=0x{r['raw']:08X}, offset={r['offset_flag']})")
                    else:
                        print("N/A")
                    print()
                    print("  Note: AMD reports a single package temperature (Tctl),")
                    print("  not per-core temps like Intel.")

                else:
                    print(f"Unknown CPU vendor '{cpu_vendor}' — cannot read temperature.")

                print()
                print(f"Updated: {time.strftime('%H:%M:%S')}  |  Ctrl+C to stop")
                time.sleep(2)

        except KeyboardInterrupt:
            print("\nStopped.")

    except OSError as e:
        print(f"ERROR: {e}")
        sys.exit(1)

    finally:
        if driver_handle and driver_handle != INVALID_HANDLE_VALUE:
            kernel32.CloseHandle(driver_handle)
        if scm_handle and svc_handle:
            stop_and_remove_driver(scm_handle, svc_handle)
        if scm_handle:
            advapi32.CloseServiceHandle(scm_handle)


if __name__ == "__main__":
    main()
