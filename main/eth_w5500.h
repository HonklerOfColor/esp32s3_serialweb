#pragma once

#include "esp_err.h"

/*
 * W5500 SPI-Ethernet — Pin-Belegung für den Seeed XIAO ESP32-S3
 *
 * Kabelverbindung W5500-Modul ↔ XIAO-Stiftleiste:
 *
 *   W5500-Pin   XIAO-Pin   GPIO
 *   ---------   --------   ----
 *   MOSI        D10        GPIO9
 *   MISO        D9         GPIO8
 *   SCLK        D8         GPIO7
 *   CS          D0         GPIO1
 *   INT         D1         GPIO2
 *   RST         D2         GPIO3   (optional; -1 = nicht angeschlossen)
 *   3V3         3V3        —
 *   GND         GND        —
 *
 * Passe W5500_CS_GPIO / W5500_INT_GPIO / W5500_RST_GPIO an deine
 * tatsächliche Verkabelung an, falls du andere Pins verwendest.
 */

#define W5500_MOSI_GPIO     9
#define W5500_MISO_GPIO     8
#define W5500_SCLK_GPIO     7
#define W5500_CS_GPIO       1   /* Chip-Select: D0 */
#define W5500_INT_GPIO      2   /* Interrupt:   D1 */
#define W5500_RST_GPIO      3   /* Reset:       D2  (-1 = nicht angeschlossen) */

#define W5500_SPI_HOST      SPI2_HOST
#define W5500_SPI_CLOCK_MHZ 20  /* W5500 unterstützt bis 80 MHz; 20 MHz ist sicher */

/**
 * @brief  W5500 SPI-Ethernet initialisieren und starten.
 *
 * Legt einen SPI2-Bus an, registriert den W5500 als Ethernet-MAC/PHY und
 * erzeugt eine esp_netif-Instanz ("ETH_DEF") mit DHCP-Client.
 *
 * IP-Events (IP_EVENT_ETH_GOT_IP) müssen vom aufrufenden Code behandelt
 * werden (Registrierung in main.c zusammen mit dem WiFi-Handler).
 *
 * @param custom_mac_str  MAC-Adresse als "XX:XX:XX:XX:XX:XX"-String oder NULL/""
 *                        für automatische Zuweisung aus eFuse.
 * @return ESP_OK bei Erfolg, sonst Fehlercode
 */
esp_err_t eth_w5500_init(const char *custom_mac_str);
