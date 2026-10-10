#ifndef HWSENSE_PORTIO_LINUX_H
#define HWSENSE_PORTIO_LINUX_H

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Raw x86 port I/O through /dev/port. The kernel only lets a process open it
 * with CAP_SYS_RAWIO (root, or `setcap cap_sys_rawio+ep` on the binary); file
 * permissions or ACLs alone are not enough.
 */
static int hwsense_port_fd = -1;

static int hwsense_port_open(char *err, size_t err_size)
{
    hwsense_port_fd = open("/dev/port", O_RDWR);
    if (hwsense_port_fd < 0) {
        snprintf(err, err_size,
                 "cannot open /dev/port (%s): direct Super I/O access needs "
                 "CAP_SYS_RAWIO, e.g. run as root or setcap cap_sys_rawio+ep",
                 strerror(errno));
        return 0;
    }
    return 1;
}

static void hwsense_port_close(void)
{
    if (hwsense_port_fd >= 0)
        close(hwsense_port_fd);
    hwsense_port_fd = -1;
}

static int hwsense_outb(unsigned port, unsigned char v)
{
    return pwrite(hwsense_port_fd, &v, 1, port) == 1;
}

static int hwsense_inb(unsigned port)
{
    unsigned char v;
    return pread(hwsense_port_fd, &v, 1, port) == 1 ? v : -1;
}
#endif

#endif
