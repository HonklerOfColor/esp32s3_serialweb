#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief  WireGuard-Client initialisieren und verbinden.
 *
 * Liest Konfiguration aus dem NVS (via config_get_current()).
 * Wird nach erstem IP-Event (WiFi oder Ethernet) aufgerufen.
 *
 * @return ESP_OK bei Erfolg, ESP_ERR_INVALID_STATE wenn nicht konfiguriert,
 *         sonstiger Fehlercode bei Init-Fehler.
 */
esp_err_t wg_client_start(void);

/**
 * @brief  WireGuard-Verbindung trennen und Ressourcen freigeben.
 */
esp_err_t wg_client_stop(void);

/**
 * @brief  Gibt true zurück, wenn der Handshake mit dem Peer erfolgreich war.
 */
bool wg_client_is_up(void);

/**
 * @brief  Gibt die konfigurierte lokale VPN-IP zurück (z.B. "10.8.0.2").
 *         Gibt "" zurück, wenn WireGuard nicht konfiguriert ist.
 */
const char *wg_client_local_ip(void);
