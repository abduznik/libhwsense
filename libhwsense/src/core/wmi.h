/*
 * wmi.h — WMI sensor access API.
 */

#ifndef WMI_H
#define WMI_H

/* Initialize/shutdown WMI */
int wmi_init(void);
void wmi_shutdown(void);

/* Thermal zones */
int wmi_read_thermal_zones(double *temps, int max_temps, char names[][64], int max_names);

/* CPU info */
typedef struct {
    char name[128];
    int max_clock_mhz;
    int current_clock_mhz;
    int voltage_mv;
    int load_percent;
    int temperature;
} wmi_cpu_info_t;

int wmi_read_cpu_info(wmi_cpu_info_t *info);

/* Fan speeds */
typedef struct {
    char name[64];
    int active_cooling;
    int desired_speed;
} wmi_fan_info_t;

int wmi_read_fans(wmi_fan_info_t *fans, int max_fans);

/* Voltage probes */
typedef struct {
    char name[64];
    double voltage;
    double max_voltage;
    double min_voltage;
} wmi_voltage_info_t;

int wmi_read_voltages(wmi_voltage_info_t *voltages, int max_voltages);

#endif /* WMI_H */
