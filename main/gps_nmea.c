#include "gps_nmea.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define NMEA_BUF_LEN 128

static char nmea_line[NMEA_BUF_LEN];
static size_t nmea_idx = 0;

void gps_nmea_init(void) {
    nmea_idx = 0;
    memset(nmea_line, 0, sizeof(nmea_line));
}

static double convert_deg_min_to_dec(const char *str, char dir) {
    double raw = atof(str);
    if (raw == 0.0) return 0.0;
    double deg = floor(raw / 100.0);
    double min = raw - (deg * 100.0);
    double dec = deg + (min / 60.0);
    if (dir == 'S' || dir == 'W') {
        dec = -dec;
    }
    return dec;
}

static void parse_nmea_sentence(const char *line, gps_data_t *gps) {
    if (strncmp(line, "$GPRMC", 6) == 0 || strncmp(line, "$GNRMC", 6) == 0) {
        // Example: $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
        char copy[NMEA_BUF_LEN];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        char *tokens[15];
        int count = 0;
        char *p = copy;
        tokens[count++] = p;

        while (*p && count < 15) {
            if (*p == ',') {
                *p = '\0';
                tokens[count++] = p + 1;
            }
            p++;
        }

        if (count >= 7) {
            char status = tokens[2][0];
            if (status == 'A') { // Valid fix
                gps->valid = true;
                gps->latitude = convert_deg_min_to_dec(tokens[3], tokens[4][0]);
                gps->longitude = convert_deg_min_to_dec(tokens[5], tokens[6][0]);
                if (count >= 8) {
                    float knots = atof(tokens[7]);
                    gps->speed_kmph = knots * 1.852f;
                }
            } else {
                gps->valid = false;
            }
        }
    } else if (strncmp(line, "$GPGGA", 6) == 0 || strncmp(line, "$GNGGA", 6) == 0) {
        // Example: $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
        char copy[NMEA_BUF_LEN];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        char *tokens[15];
        int count = 0;
        char *p = copy;
        tokens[count++] = p;

        while (*p && count < 15) {
            if (*p == ',') {
                *p = '\0';
                tokens[count++] = p + 1;
            }
            p++;
        }

        if (count >= 8) {
            gps->satellites = atoi(tokens[7]);
            int fix = atoi(tokens[6]);
            if (fix > 0) {
                gps->valid = true;
                if (strlen(tokens[2]) > 0 && strlen(tokens[4]) > 0) {
                    gps->latitude = convert_deg_min_to_dec(tokens[2], tokens[3][0]);
                    gps->longitude = convert_deg_min_to_dec(tokens[4], tokens[5][0]);
                }
            }
        }
    }
}

void gps_nmea_parse_char(char c, gps_data_t *gps_out) {
    if (c == '$') {
        nmea_idx = 0;
        nmea_line[nmea_idx++] = c;
    } else if (nmea_idx > 0 && nmea_idx < NMEA_BUF_LEN - 1) {
        if (c == '\r' || c == '\n') {
            nmea_line[nmea_idx] = '\0';
            parse_nmea_sentence(nmea_line, gps_out);
            nmea_idx = 0;
        } else {
            nmea_line[nmea_idx++] = c;
        }
    } else {
        nmea_idx = 0;
    }
}

