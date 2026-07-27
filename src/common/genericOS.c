/*
 * genericOS.c — Documented stubs for custom/embedded OS support.
 *
 * This file provides the interface that a port to a non-Windows OS must
 * implement.  The library's vendor-specific code (intel.c, amd_zen.c)
 * calls these functions instead of Win32 APIs directly.
 *
 * To port libhwsense to a new OS:
 *   1. Copy this file to os_yourplatform.c
 *   2. Implement every function declared here
 *   3. Add your .c file to CMakeLists.txt (or your build system)
 *   4. The vendor-specific code (intel.c, amd_zen.c) should not need changes
 *
 * What each function must do:
 *   - os_init / os_shutdown: OS-specific resource setup/teardown
 *   - os_read_msr: Execute RDMSR on a specific logical core
 *   - os_read_pci_config: Read a DWORD from PCI config space
 *   - os_write_pci_config: Write a DWORD to PCI config space
 *   - os_pin_thread / os_restore_thread: CPU core affinity for MSR reads
 *   - os_sleep_ms: Sleep for N milliseconds
 */

#include <stdint.h>

#ifndef _WIN32

/* ── Forward declarations (called by driver.c and cpu/*.c) ─────────── */

/*
 * Initialize OS-specific resources.
 * On Windows this installs the WinRing0 driver service.
 * On Linux this would open /dev/cpu/0/msr, etc.
 * Returns 0 on success, negative on error.
 */
int os_init(void);

/*
 * Shut down OS-specific resources and release handles.
 */
void os_shutdown(void);

/*
 * Read a Model-Specific Register on the given logical core.
 *
 * Parameters:
 *   core_id   — logical processor index (0-based)
 *   msr_index — MSR address (e.g. 0x19C for IA32_THERM_STATUS)
 *   out_value — receives the 64-bit MSR value
 *
 * Returns 0 on success, negative on error.
 *
 * On Linux: pread(/dev/cpu/<core_id>/msr, &val, 8, msr_index)
 * On Windows: DeviceIoControl(IOCTL_OLS_READ_MSR) after pinning thread
 * On bare metal: WRMSR/RDMSR instructions (requires ring-0)
 */
int os_read_msr(int core_id, uint32_t msr_index, uint64_t *out_value);

/*
 * Read a 32-bit value from PCI config space.
 *
 * Parameters:
 *   bus, device, function — PCI BDF address
 *   reg_offset            — config space register offset (0-255)
 *   out_value             — receives the 32-bit value
 *
 * Returns 0 on success, negative on error.
 *
 * On Linux: pread(/sys/bus/pci/devices/XX:XX.X/config, &val, 4, offset)
 * On Windows: DeviceIoControl(IOCTL_OLS_READ_PCI_CONFIG)
 * On bare metal: IN/OUT to config space I/O ports (0xCF8/0xCFC)
 */
int os_read_pci_config(int bus, int device, int function,
                       uint32_t reg_offset, uint32_t *out_value);

/*
 * Write a 32-bit value to PCI config space.
 *
 * Parameters:
 *   bus, device, function — PCI BDF address
 *   reg_offset            — config space register offset (0-255)
 *   value                 — 32-bit value to write
 *
 * Returns 0 on success, negative on error.
 */
int os_write_pci_config(int bus, int device, int function,
                        uint32_t reg_offset, uint32_t value);

/*
 * Pin the calling thread to a specific logical core.
 * Returns an opaque handle/cookie that must be passed to os_restore_thread.
 *
 * On Linux: sched_setaffinity(pthread_self(), sizeof(mask), &mask)
 * On Windows: SetThreadAffinityMask(GetCurrentThread(), 1 << core_id)
 * On bare metal: no-op (single core or manually managed)
 */
uintptr_t os_pin_thread(int core_id);

/*
 * Restore thread affinity to its previous state.
 * handle is the value returned by os_pin_thread.
 */
void os_restore_thread(uintptr_t handle);

/*
 * Sleep for the given number of milliseconds.
 * Used in SMU mailbox polling loops.
 */
void os_sleep_ms(int ms);

#endif /* !_WIN32 */
#endif /* HWSENSE_INTERNAL_H */
