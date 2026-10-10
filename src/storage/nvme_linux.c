/*
 * nvme_linux.c — NVMe SMART / Health Information log on Linux.
 *
 * Issues an NVMe Get Log Page admin command (opcode 0x02, log ID 0x02)
 * through the NVME_IOCTL_ADMIN_CMD ioctl on /dev/nvmeN and decodes the
 * 512-byte log per the NVMe Base Specification, "SMART / Health Information".
 * Read-only, but the kernel requires CAP_SYS_ADMIN and access to the device
 * node, so in practice root.
 */

#ifndef _WIN32

#include "../core/hwsense_internal.h"
#include "nvme_linux.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/nvme_ioctl.h>

/* Little-endian field of up to 8 bytes (the log fields are 128-bit; the high
 * half is never non-zero in practice and is dropped). */
static unsigned long long le(const unsigned char *p, int bytes)
{
    unsigned long long v = 0;
    int i;
    for (i = bytes - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

void hwsense_nvme_smart_decode(const unsigned char *b, hwsense_nvme_smart_t *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->critical_warning = b[0];
    o->composite_celsius = (double)le(b + 1, 2) - 273.15;
    o->available_spare_pct = b[3];
    o->available_spare_threshold_pct = b[4];
    o->percentage_used = b[5];
    o->data_units_read = le(b + 32, 8);
    o->data_units_written = le(b + 48, 8);
    o->host_read_commands = le(b + 64, 8);
    o->host_write_commands = le(b + 80, 8);
    o->controller_busy_minutes = le(b + 96, 8);
    o->power_cycles = le(b + 112, 8);
    o->power_on_hours = le(b + 128, 8);
    o->unsafe_shutdowns = le(b + 144, 8);
    o->media_errors = le(b + 160, 8);
    o->error_log_entries = le(b + 176, 8);
    o->warning_temp_minutes = (unsigned int)le(b + 192, 4);
    o->critical_temp_minutes = (unsigned int)le(b + 196, 4);

    for (i = 0; i < 8; i++) {
        unsigned long long k = le(b + 200 + i * 2, 2);
        if (k)                                  /* 0 = sensor not implemented */
            o->sensor_celsius[o->sensor_count++] = (double)k - 273.15;
    }
    o->ok = 1;
}

int hwsense_nvme_smart(int index, hwsense_nvme_smart_t *out)
{
    unsigned char buf[512];
    struct nvme_admin_cmd cmd;
    char path[32];
    int fd;

    memset(out, 0, sizeof(*out));
    snprintf(path, sizeof(path), "/dev/nvme%d", index);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(out->error, sizeof(out->error),
                 "cannot open %s (%s): NVMe SMART needs root or CAP_SYS_ADMIN",
                 path, strerror(errno));
        return 0;
    }

    memset(buf, 0, sizeof(buf));
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = 0x02;                       /* Get Log Page */
    cmd.nsid = 0xffffffff;                   /* controller-wide */
    cmd.addr = (uint64_t)(uintptr_t)buf;
    cmd.data_len = sizeof(buf);
    cmd.cdw10 = 0x02 | ((unsigned)(sizeof(buf) / 4 - 1) << 16);   /* LID 2, NUMD */

    if (ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) != 0) {
        snprintf(out->error, sizeof(out->error),
                 "NVMe Get Log Page failed on %s (%s)", path, strerror(errno));
        close(fd);
        return 0;
    }
    close(fd);

    hwsense_nvme_smart_decode(buf, out);
    return 1;
}

#endif /* !_WIN32 */
