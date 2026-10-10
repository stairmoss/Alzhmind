#ifndef GPS_NMEA_H
#define GPS_NMEA_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool valid;
    double latitude;
    double longitude;
    float speed_kmph;
    int satellites;
} gps_data_t;

void gps_nmea_init(void);
void gps_nmea_parse_char(char c, gps_data_t *gps_out);

#endif // GPS_NMEA_H

