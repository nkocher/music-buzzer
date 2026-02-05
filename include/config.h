#pragma once

#include "secrets.h"  // WIFI_SSID, WIFI_PASS (gitignored)

// Static IP
#define BUZZER_IP 192, 168, 0, 201
#define GATEWAY   192, 168, 0, 1
#define SUBNET    255, 255, 255, 0

// Port
#define SERVER_PORT 80

// Buzzer pins (GPIO 4-7 on left side of ESP32-S3 38-pin board)
#define PIN_BUZ0  4
#define PIN_BUZ1  5
#define PIN_BUZ2  6
#define PIN_BUZ3  7
#define NUM_BUZZERS 4

// LEDC channels (one per buzzer, each gets its own timer)
#define LEDC_RESOLUTION 10

// Stop button (HW-483 module)
#define PIN_STOP_BTN 15

// Timing
#define MELODY_LOOP_PAUSE_MS  400
#define STATE_SETTLE_MS       200
#define WIFI_CHECK_INTERVAL   10000
#define MAX_NOTES_PER_SONG    256
