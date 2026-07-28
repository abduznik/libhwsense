"""
hwsense.py — Python wrapper for libhwsense DLL.

Usage:
    from hwsense import Hwsense
    
    with Hwsense() as hw:
        print(f"CPU: {hw.get_cpu_temp():.1f}°C")
        print(f"GPU: {hw.get_gpu_info()['temperature']}°C")
"""

import ctypes
import os

# Define struct types at module level
class TempResult(ctypes.Structure):
    _fields_ = [
        ("ok", ctypes.c_int),
        ("celsius", ctypes.c_double),
        ("error", ctypes.c_char * 256)
    ]

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

class CpuLoad(ctypes.Structure):
    _fields_ = [
        ("percent", ctypes.c_double),
        ("num_cpus", ctypes.c_int)
    ]

class MemStats(ctypes.Structure):
    _fields_ = [
        ("total_gb", ctypes.c_double),
        ("used_gb", ctypes.c_double),
        ("free_gb", ctypes.c_double),
        ("available_gb", ctypes.c_double),
        ("swap_used_gb", ctypes.c_double),
        ("percent_used", ctypes.c_int)
    ]

class Uptime(ctypes.Structure):
    _fields_ = [
        ("days", ctypes.c_int),
        ("hours", ctypes.c_int),
        ("minutes", ctypes.c_int),
        ("seconds", ctypes.c_int)
    ]


class Hwsense:
    """Python wrapper for libhwsense hardware sensor library."""
    
    def __init__(self, dll_path=None):
        """Initialize the library and load WinRing0 driver."""
        if dll_path is None:
            dll_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hwsense.dll")
        
        if not os.path.exists(dll_path):
            raise FileNotFoundError(f"hwsense.dll not found at {dll_path}")
        
        self.dll = ctypes.CDLL(dll_path)
        self.ctx = None
        self._setup_types()
        
        self.ctx = self.dll.hwsense_init()
        if not self.ctx:
            raise RuntimeError("Failed to initialize hwsense - run as Administrator!")
    
    def _setup_types(self):
        """Set function signatures."""
        self.dll.hwsense_init.restype = ctypes.c_void_p
        self.dll.hwsense_init.argtypes = []
        
        self.dll.hwsense_shutdown.restype = None
        self.dll.hwsense_shutdown.argtypes = [ctypes.c_void_p]
        
        self.dll.hwsense_cpu_package_temp.restype = TempResult
        self.dll.hwsense_cpu_package_temp.argtypes = [ctypes.c_void_p]
        
        self.dll.hwsense_cpu_core_clock.restype = ctypes.c_int
        self.dll.hwsense_cpu_core_clock.argtypes = [ctypes.c_void_p]
        
        self.dll.hwsense_cpu_core_voltage_value.restype = ctypes.c_double
        self.dll.hwsense_cpu_core_voltage_value.argtypes = [ctypes.c_void_p]
        
        self.dll.hwsense_cpu_package_power_watts.restype = ctypes.c_double
        self.dll.hwsense_cpu_package_power_watts.argtypes = [ctypes.c_void_p]
        
        self.dll.hwsense_gpu_info.restype = GpuInfo
        self.dll.hwsense_gpu_info.argtypes = [ctypes.c_int]
        
        self.dll.win_get_cpu_load_sampled.restype = ctypes.c_int
        self.dll.win_get_cpu_load_sampled.argtypes = [ctypes.POINTER(CpuLoad)]
        
        self.dll.win_get_mem_stats.restype = ctypes.c_int
        self.dll.win_get_mem_stats.argtypes = [ctypes.POINTER(MemStats)]
        
        self.dll.win_get_uptime.restype = Uptime
        self.dll.win_get_uptime.argtypes = []
    
    def get_cpu_temp(self):
        """Get CPU temperature in Celsius."""
        result = self.dll.hwsense_cpu_package_temp(self.ctx)
        if result.ok:
            return result.celsius
        return None
    
    def get_cpu_clock(self):
        """Get CPU clock speed in MHz."""
        return self.dll.hwsense_cpu_core_clock(self.ctx)
    
    def get_cpu_voltage(self):
        """Get CPU voltage in volts."""
        return self.dll.hwsense_cpu_core_voltage_value(self.ctx)
    
    def get_cpu_power(self):
        """Get CPU package power in watts."""
        return self.dll.hwsense_cpu_package_power_watts(self.ctx)
    
    def get_gpu_info(self, gpu_index=0):
        """Get GPU info including temperature, VRAM, power, load."""
        info = self.dll.hwsense_gpu_info(gpu_index)
        if info.ok:
            return {
                "name": info.name.decode("utf-8", errors="ignore"),
                "temperature": info.temperature,
                "vram_used_mb": info.vram_used_mb,
                "vram_total_mb": info.vram_total_mb,
                "power_usage_w": info.power_usage_w,
                "gpu_load": info.gpu_load,
                "mem_load": info.mem_load,
                "clock_mhz": info.clock_mhz,
                "mem_clock_mhz": info.mem_clock_mhz
            }
        return None
    
    def get_cpu_load(self):
        """Get CPU load percentage (samples for 1 second)."""
        load = CpuLoad()
        if self.dll.win_get_cpu_load_sampled(ctypes.byref(load)):
            return load.percent
        return None
    
    def get_memory(self):
        """Get memory usage info."""
        mem = MemStats()
        if self.dll.win_get_mem_stats(ctypes.byref(mem)):
            return {
                "total_gb": mem.total_gb,
                "used_gb": mem.used_gb,
                "percent": mem.percent_used
            }
        return None
    
    def get_uptime(self):
        """Get system uptime."""
        uptime = self.dll.win_get_uptime()
        return {
            "days": uptime.days,
            "hours": uptime.hours,
            "minutes": uptime.minutes
        }
    
    def print_report(self):
        """Print a formatted sensor report."""
        print("=== Hardware Sensor Report ===\n")
        
        print("--- CPU ---")
        temp = self.get_cpu_temp()
        if temp is not None:
            print(f"  Temperature:    {temp:.1f}°C")
        
        clock = self.get_cpu_clock()
        if clock > 0:
            print(f"  Clock:          {clock} MHz")
        
        voltage = self.get_cpu_voltage()
        if voltage > 0:
            print(f"  Voltage:        {voltage:.3f} V")
        
        power = self.get_cpu_power()
        if power > 0:
            print(f"  Power:          {power:.2f} W")
        
        load = self.get_cpu_load()
        if load is not None:
            print(f"  Load:           {load:.1f}%")
        print()
        
        gpu = self.get_gpu_info()
        if gpu:
            print("--- GPU ---")
            print(f"  Name:           {gpu['name']}")
            print(f"  Temperature:    {gpu['temperature']}°C")
            if gpu['vram_total_mb'] > 0:
                print(f"  VRAM:           {gpu['vram_used_mb']}/{gpu['vram_total_mb']} MB")
            if gpu['power_usage_w'] > 0:
                print(f"  Power:          {gpu['power_usage_w']} W")
            print()
        
        mem = self.get_memory()
        if mem:
            print("--- Memory ---")
            print(f"  Used:           {mem['used_gb']:.1f} / {mem['total_gb']:.1f} GB ({mem['percent']}%)")
            print()
        
        uptime = self.get_uptime()
        print("--- System ---")
        print(f"  Uptime:         {uptime['days']}d {uptime['hours']}h {uptime['minutes']}m")
    
    def close(self):
        """Shutdown and cleanup."""
        if self.ctx:
            self.dll.hwsense_shutdown(self.ctx)
            self.ctx = None
    
    def __del__(self):
        self.close()
    
    def __enter__(self):
        return self
    
    def __exit__(self, *args):
        self.close()


if __name__ == "__main__":
    with Hwsense() as hw:
        hw.print_report()
