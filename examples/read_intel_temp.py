import struct, os, sys

MSR_IA32_THERM_STATUS = 0x19C
MSR_IA32_TEMPERATURE_TARGET = 0x1A2

def read_msr(core, msr):
    with open("/dev/cpu/%d/msr" % core, "rb") as f:
        f.seek(msr)
        return struct.unpack("Q", f.read(8))[0]

cores = sorted([int(x) for x in os.listdir("/dev/cpu") if x.isdigit()])
print("CPU: Intel 12th Gen i5-1235U")
print("Cores: %d" % len(cores))
print()

# Read TjMax from core 0
val = read_msr(0, MSR_IA32_TEMPERATURE_TARGET)
tj_max = (val >> 16) & 0xFF
print("TjMax (MSR 0x1A2): %d C" % tj_max)
print()

print("%-6s  %-10s  %-10s  %-10s" % ("Core", "Valid", "Readout", "Temp (C)"))
print("-" * 42)

for core in cores:
    try:
        val = read_msr(core, MSR_IA32_THERM_STATUS)
        valid = bool(val & (1 << 31))
        readout = (val >> 16) & 0x7F
        if valid:
            temp = tj_max - readout
            print("%-6d  %-10s  %-10d  %-10.1f" % (core, "Yes", readout, temp))
        else:
            print("%-6d  %-10s  %-10s  %-10s" % (core, "No", "---", "N/A"))
    except Exception as e:
        print("%-6d  %-10s  %-10s  %-10s" % (core, "ERR", "---", str(e)[:20]))
