# libhwsense Roadmap

## v0.1.0 (Current Release)

### Completed Features

| Feature | Status | Notes |
|---------|--------|-------|
| CPU Temperature (Intel/AMD) | ✅ | MSR/SMN access via WinRing0 |
| CPU Voltage | ✅ | AMD SVI2, Intel MSR, WMI |
| CPU Frequency | ✅ | AMD MSR, Intel MSR |
| CPU Power | ✅ | AMD SVI2, Intel RAPL |
| GPU Temperature | ✅ | NVIDIA NVML, AMD ADL |
| GPU VRAM/Power | ✅ | NVIDIA NVML |
| NVMe Temperature | ✅ | SMART via IOCTL |
| Super I/O Support | ✅ | Nuvoton/ITE/Winbond/Fintek |
| WMI Sensor Queries | ✅ | ROOT\WMI, ROOT\CIMV2 |
| System Stats | ✅ | Memory, CPU load, disk, uptime |
| Python Wrapper | ✅ | ctypes-based DLL wrapper |
| Unified API | ✅ | Cross-vendor transparent access |
| CPU Diagnostics | ✅ | Feature detection, warnings |
| Security Layers | ✅ | IO port validation, audit logging |

---

## v0.2.0 (Next Release)

### Core Features

#### 1. Linux Support (#1)
- CPU temperature via /sys/class/thermal/
- CPU frequency via /sys/devices/system/cpu/
- GPU temperature via nvidia-smi/sysfs
- Fan speeds via lm-sensors
- Memory/CPU/disk stats via /proc

#### 2. NVMe Health Monitoring (#2)
- Wear level (percentage used)
- Power-on hours
- Total bytes read/written
- Error count

#### 3. Network Interface Monitoring (#3)
- RX/TX bytes per second
- Packets per second
- Error count

#### 4. Disk I/O Performance (#4)
- Read/write bytes per second
- IOPS

#### 5. Temperature Alert System (#5)
- Per-sensor thresholds
- Callback functions

### Hardware Support

#### 6. More CPU Families (#11)
- Intel 13th/14th Gen
- AMD Zen 4/5

#### 7. More Super I/O Chips (#10)
- Nuvoton NCT6687D
- ITE IT8655E/IT8665E/IT8686E
- Fintek F71869A/F71811

#### 8. ASUS WMI Support (#7)
- ASUS-specific sensor access

#### 9. Intel Arc GPU (#29)
- Intel GPU temperature monitoring

#### 10. AMD APU GPU (#22)
- AMD integrated GPU temperature

### Improvements

#### 11. GPU Features
- GPU fan speed (#13)
- GPU memory clock (#15)
- Fix GPU power limit (#6)

#### 12. Documentation (#14)
- API reference
- Troubleshooting guide

#### 13. Data Export (#8)
- CSV/JSON export

#### 14. Error Messages (#12)
- Better unsupported CPU messages

#### 15. CPU Microcode (#18)
- Version detection

#### 16. Process Monitoring (#16)
- Top processes by CPU/memory

#### 17. Benchmarking (#28)
- Performance testing

#### 18. Linux NVMe (#19)
- NVMe temperature on Linux

---

## v0.3.0 (Future)

### Long-term Goals

- Plugin system for third-party sensors
- More hardware support
- Performance optimizations

---

## Contributing

We welcome contributions! Check the [issues](https://github.com/abduznik/libhwsense/issues) for tasks tagged with `good first issue` or `help wanted`.
