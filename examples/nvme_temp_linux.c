/*
 * nvme_temp_linux.c - NVMe drive temperature on Linux via sysfs hwmon.
 *
 * Rather than assuming where the nvme driver's hwmon device sits in the sysfs tree (that nesting has proven inconsistent across kernels), this walks every entry in /sys/class/hwmon/ and checks its "name" file. 
 *
 * The nvme driver always registers with the literal name "nvme" (see drivers/nvme/host/hwmon.c, hwmon_device_register_with_info), so matching on that string is the same method lm-sensors and udev use, and it is independent of drive count, PCI topology, or kernel version - as long as CONFIG_NVME_HWMON is enabled (kernel >= 5.5).
 *
 * No root required - hwmon sysfs files are normally world-readable, unlike the MSR reads in intel_sensors_linux.c.
 *
 * Compile: gcc -O2 -o nvme_temp nvme_temp_linux.c
 * Run:     ./nvme_temp
 */

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<dirent.h>
#include<fcntl.h>
#include<unistd.h>
#include<limits.h>

#define HWMON_CLASS_DIR "/sys/class/hwmon"

/* Read a small one-line sysfs file, trimming the trailing newline. */
static int read_line(const char *path, char *out, size_t out_sz){
  int fd, n;
  fd= open(path, O_RDONLY);
  if(fd<0) return -1;

  n= read(fd, out, out_sz-1);
  close(fd);

  if(n<=0) return -1;

  out[n]= '\0';
  if(out[n-1]=='\n')  out[n-1]= '\0';

  return 0;
}

/* Read temp1_input (millidegrees C) from a hwmon dir. Returns C, or -1. */
static int read_temp1_input(const char *hwmon_path){
  char path[PATH_MAX];
  char buf[32];
  int len;

  len= snprintf(path, sizeof(path), "%s/temp1_input", hwmon_path);

  if(len<0 || (size_t)len>=sizeof(path))  return -1;
  if(read_line(path, buf, sizeof(buf)) < 0)  return -1;

  return atoi(buf) / 1000;
}

/*
 * Best-effort label for which physical drive a hwmon entry belongs to.
 * Follows the "device" symlink and pulls out the "nvmeN" component if present; falls back to the hwmon entry's own name otherwise.
 */
static void label_drive(const char *hwmon_name, char *out, size_t out_sz){
  char device_link[PATH_MAX], target[PATH_MAX];
  ssize_t n;
  const char *p;

  snprintf(device_link, sizeof(device_link), "%s/%s/device", HWMON_CLASS_DIR, hwmon_name);

  n= readlink(device_link, target, sizeof(target)-1);
  if(n<=0){
    if(snprintf(out, out_sz, "%.*s", (int)out_sz-1, hwmon_name)<0)  out[0]= '\0';
    return;
  }
  target[n]= '\0';

  p= strstr(target, "/nvme");
  if(p){
    p+=1; /* skip the leading slash */
    snprintf(out, out_sz, "%.*s", (int)strcspn(p, "/"), p);
    return;
  }

  if(snprintf(out, out_sz, "%.*s", (int)out_sz-1, hwmon_name)<0)  out[0]= '\0';
}

int main(void){
  DIR *d;
  struct dirent *entry;
  int found= 0;

  printf("NVMe Temperature (Linux hwmon sysfs)\n");
  printf("=====================================\n\n");

  d= opendir(HWMON_CLASS_DIR);
  if(!d){
    fprintf(stderr, "Cannot open %s - is sysfs mounted?\n", HWMON_CLASS_DIR);
    return 1;
  }

  while((entry = readdir(d))!=NULL){
    char name_path[PATH_MAX];
    char hwmon_path[PATH_MAX];
    char name[32];
    char label[64];
    int temp, len;

    if(entry->d_name[0]=='.')  continue;

    len= snprintf(name_path, sizeof(name_path), "%s/%s/name", HWMON_CLASS_DIR, entry->d_name);
    if(len<0 || (size_t)len>=sizeof(name_path)) continue;

    if(read_line(name_path, name, sizeof(name))<0) continue;

    if(strcmp(name, "nvme")!=0) continue; /* not an nvme hwmon device, skip */

    len= snprintf(hwmon_path, sizeof(hwmon_path), "%s/%s", HWMON_CLASS_DIR, entry->d_name);
    if(len<0 || (size_t)len>=sizeof(hwmon_path)) continue;

    label_drive(entry->d_name, label, sizeof(label));

    temp= read_temp1_input(hwmon_path);
    if(temp>=0){
      printf("%-10s: %d C   (%s)\n", label, temp, hwmon_path);
      found++;
    } 
    else  fprintf(stderr, "%-10s: ERROR reading temp1_input\n", label);
  }

  closedir(d);

  if(!found){
    fprintf(stderr, "\nNo NVMe hwmon temperature sensors found.\n");
    fprintf(stderr, "This system may predate CONFIG_NVME_HWMON (kernel < 5.5),\n");
    fprintf(stderr, "or the drive's controller may not report a Health Info Log.\n");
    fprintf(stderr, "Fallback: try 'sudo smartctl -a /dev/nvme0 -j' instead.\n");
    return 1;
  }

  printf("\nDone.\n");
  return 0;
}
