#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define CONFIG_NAMESPACE "cisco_oob"

typedef struct {
    /* WiFi STA */
    char ssid[33];
    char password[65];
    uint32_t baud_rate;

    /* WiFi AP-Fallback */
    char ap_ssid[33];
    char ap_pass[65];

    /* MAC-Adressen (leer = automatisch aus eFuse) */
    char wifi_mac[18];   /* "XX:XX:XX:XX:XX:XX" oder "" */
    char eth_mac[18];    /* "XX:XX:XX:XX:XX:XX" oder "" */

    /* WireGuard */
    bool     wg_enabled;
    char     wg_privkey[48];      /* Base64-codierter privater Schlüssel des ESP32 (44 Zeichen) */
    char     wg_peer_pubkey[48];  /* Base64-codierter öffentlicher Schlüssel des WG-Servers */
    char     wg_endpoint[64];     /* Öffentliche IP / Hostname des WG-Servers */
    uint16_t wg_port;             /* WireGuard-Port (Standard: 51820) */
    char     wg_local_ip[20];     /* VPN-IP des ESP32 (z.B. "10.8.0.2")          */
    char     wg_local_mask[20];   /* VPN-Subnetzmaske    (z.B. "255.255.255.0")  */
    uint8_t  wg_keepalive;        /* Keepalive in Sekunden (Standard: 25)         */
} app_config_t;

esp_err_t config_init(void);
esp_err_t config_load(app_config_t *cfg);
esp_err_t config_save(const app_config_t *cfg);
const app_config_t* config_get_current(void);
