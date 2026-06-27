/*
 * WireGuard-Client für den ESP32-S3 OOB-Konsolen-Server
 *
 * Verwendet trombik/esp_wireguard (wireguard-lwIP).
 * Konfiguration wird über die Web-UI unter /wg gespeichert.
 *
 * Begriffe / Mapping auf WireGuard-Konfig:
 *   wg_privkey     → [Interface] PrivateKey (ESP32-Schlüssel)
 *   wg_peer_pubkey → [Peer] PublicKey       (Server-Schlüssel)
 *   wg_endpoint    → [Peer] Endpoint        (öffentliche IP/Host des Servers)
 *   wg_port        → [Peer] Endpoint-Port   (Standard 51820)
 *   wg_local_ip    → [Interface] Address    (VPN-IP des ESP32, z.B. 10.8.0.2)
 *   wg_local_mask  → Subnetzmaske dazu      (z.B. 255.255.255.0)
 *   wg_keepalive   → [Peer] PersistentKeepalive
 *
 * HINWEIS zum trombik/esp_wireguard-API:
 *   allowed_ip / allowed_ip_mask in wireguard_config_t sind entgegen dem
 *   irreführenden Namen die LOKALE IP des WireGuard-Interfaces (= [Interface] Address),
 *   NICHT die AllowedIPs-Route. Routing über das Tunnel-Interface wird ggf.
 *   durch esp_wireguard_set_default() gesetzt.
 */

#include "wg_client.h"
#include "config.h"

#include "esp_wireguard.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "wg_client";

/* Statische Puffer — Zeiger in wireguard_config_t müssen für die
 * gesamte Verbindungsdauer gültig bleiben.                          */
static char s_privkey[48];
static char s_peer_pubkey[48];
static char s_endpoint[64];
static char s_local_ip[20];
static char s_local_mask[20];

static wireguard_config_t s_wg_cfg;
static wireguard_ctx_t    s_wg_ctx;
static bool               s_running = false;

/* ------------------------------------------------------------------ */

static void wg_monitor_task(void *arg)
{
    uint32_t last_state = 0;
    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        bool up = (esp_wireguardif_peer_is_up(&s_wg_ctx) == ESP_OK);
        if ((uint32_t)up != last_state) {
            last_state = (uint32_t)up;
            if (up) {
                ESP_LOGI(TAG, "Peer verbunden ✓ (VPN-IP: %s)", s_local_ip);
            } else {
                ESP_LOGW(TAG, "Peer nicht erreichbar — warte auf Handshake...");
            }
        }
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */

esp_err_t wg_client_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Bereits gestartet");
        return ESP_OK;
    }

    const app_config_t *cfg = config_get_current();

    if (!cfg->wg_enabled) {
        ESP_LOGI(TAG, "WireGuard deaktiviert — übersprungen");
        return ESP_OK;
    }
    if (cfg->wg_privkey[0] == '\0') {
        ESP_LOGW(TAG, "Privater Schlüssel fehlt — /wg konfigurieren");
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->wg_peer_pubkey[0] == '\0') {
        ESP_LOGW(TAG, "Server-Pubkey fehlt — /wg konfigurieren");
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->wg_endpoint[0] == '\0') {
        ESP_LOGW(TAG, "Server-Endpoint fehlt — /wg konfigurieren");
        return ESP_ERR_INVALID_STATE;
    }

    /* Strings in statische Puffer kopieren */
    strlcpy(s_privkey,    cfg->wg_privkey,     sizeof(s_privkey));
    strlcpy(s_peer_pubkey, cfg->wg_peer_pubkey, sizeof(s_peer_pubkey));
    strlcpy(s_endpoint,   cfg->wg_endpoint,    sizeof(s_endpoint));
    strlcpy(s_local_ip,
            cfg->wg_local_ip[0] ? cfg->wg_local_ip : "10.8.0.2",
            sizeof(s_local_ip));
    strlcpy(s_local_mask,
            cfg->wg_local_mask[0] ? cfg->wg_local_mask : "255.255.255.0",
            sizeof(s_local_mask));

    /* WireGuard-Konfiguration aufbauen */
    s_wg_cfg = (wireguard_config_t)ESP_WIREGUARD_CONFIG_DEFAULT();
    s_wg_cfg.private_key          = s_privkey;
    s_wg_cfg.public_key           = s_peer_pubkey;
    s_wg_cfg.endpoint             = s_endpoint;
    s_wg_cfg.port                 = cfg->wg_port ? cfg->wg_port : 51820;
    s_wg_cfg.allowed_ip           = s_local_ip;    /* = lokale VPN-IP des ESP32 */
    s_wg_cfg.allowed_ip_mask      = s_local_mask;
    s_wg_cfg.persistent_keepalive = cfg->wg_keepalive ? cfg->wg_keepalive : 25;

    ESP_LOGI(TAG, "Initialisiere WireGuard: %s:%d  VPN-IP=%s",
             s_endpoint, s_wg_cfg.port, s_local_ip);

    esp_err_t ret = esp_wireguard_init(&s_wg_cfg, &s_wg_ctx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wireguard_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wireguard_connect(&s_wg_ctx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wireguard_connect: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = true;
    xTaskCreate(wg_monitor_task, "wg_mon", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "WireGuard gestartet — warte auf Handshake mit %s", s_endpoint);
    return ESP_OK;
}

esp_err_t wg_client_stop(void)
{
    if (!s_running) return ESP_OK;

    s_running = false;
    esp_err_t ret = esp_wireguard_disconnect(&s_wg_ctx);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WireGuard getrennt");
    }
    return ret;
}

bool wg_client_is_up(void)
{
    if (!s_running) return false;
    return esp_wireguardif_peer_is_up(&s_wg_ctx) == ESP_OK;
}

const char *wg_client_local_ip(void)
{
    if (!s_running) return "";
    return s_local_ip;
}
