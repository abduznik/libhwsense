#ifndef HWSENSE_NVME_LINUX_H
#define HWSENSE_NVME_LINUX_H

#include "../../include/hwsense.h"

#ifndef _WIN32
/* Decode a 512-byte SMART / Health Information log. Exposed for tests. */
void hwsense_nvme_smart_decode(const unsigned char *log512, hwsense_nvme_smart_t *out);
#endif

#endif
