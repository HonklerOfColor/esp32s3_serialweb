#include "config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config";

static app_config_t current_config;

bool config_ap_pass_valid(const char *pass)
{
    return pass && strlen(pass) >= AP_PASS_MIN_LEN;
}

void config_normalize_ap_pass(char *pass, size_t size)
{
    if (!pass || size == 0) return;
    if (!config_ap_pass_valid(pass)) {
        if (pass[0] != '\0')
            ESP_LOGW(TAG, "AP-Passwort ungültig (%u Zeichen) — %s",
                     (unsigned)strlen(pass), AP_PASS_DEFAULT);
        strncpy(pass, AP_PASS_DEFAULT, size - 1);
        pass[size - 1] = '\0';
    }
}

esp_err_t config_set_boot_ap(bool enable)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "boot_ap", enable ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool config_consume_boot_ap(void)
{
    nvs_handle_t handle;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;

    uint8_t v = 0;
    if (nvs_get_u8(handle, "boot_ap", &v) != ESP_OK || v == 0) {
        nvs_close(handle);
        return false;
    }

    nvs_erase_key(handle, "boot_ap");
    nvs_commit(handle);
    nvs_close(handle);
    return true;
}

esp_err_t config_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t config_load(app_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Kein Config in NVS — Standardwerte");
        strncpy(cfg->ssid,     "net1",             sizeof(cfg->ssid)     - 1);
        strncpy(cfg->password, "GEcw5gRacTDWDvdb", sizeof(cfg->password) - 1);
        cfg->baud_rate = 9600;
        cfg->otg_enabled = false;
        strncpy(cfg->ap_ssid, "ESP32S3_AP",   sizeof(cfg->ap_ssid) - 1);
        strncpy(cfg->ap_pass, "DefaultPass!", sizeof(cfg->ap_pass) - 1);
        cfg->wifi_mac[0] = '\0';
        cfg->eth_mac[0]  = '\0';
        /* WireGuard-Standardwerte */
        cfg->wg_enabled = false;
        strncpy(cfg->wg_privkey,     "aLhs9v1mLl6CY9MwzCzI0/ER9d330ToTwQRBT/cfCUo=", sizeof(cfg->wg_privkey)     - 1);
        strncpy(cfg->wg_peer_pubkey, "mTl5spEhdOIr5OXraSrs9epvUc8d0wwLRoyz18kPTRs=", sizeof(cfg->wg_peer_pubkey) - 1);
        strncpy(cfg->wg_endpoint,    "192.168.1.89",   sizeof(cfg->wg_endpoint)    - 1);
        cfg->wg_port = 51820;
        strncpy(cfg->wg_local_ip,   "10.8.0.3",       sizeof(cfg->wg_local_ip)   - 1);
        strncpy(cfg->wg_local_mask, "255.255.255.0",   sizeof(cfg->wg_local_mask) - 1);
        cfg->wg_keepalive = 25;
        memcpy(&current_config, cfg, sizeof(app_config_t));
        return ESP_OK;
    }

    /* ---- WiFi ---- */
    size_t len;
    len = sizeof(cfg->ssid);
    if (nvs_get_str(handle, "ssid", cfg->ssid, &len) != ESP_OK)
        strncpy(cfg->ssid, "net1", sizeof(cfg->ssid) - 1);

    len = sizeof(cfg->password);
    if (nvs_get_str(handle, "password", cfg->password, &len) != ESP_OK ||
        cfg->password[0] == '\0') {
        if (cfg->password[0] == '\0')
            ESP_LOGW(TAG, "WLAN-Passwort in NVS leer — Standardwert wird verwendet");
        strncpy(cfg->password, "GEcw5gRacTDWDvdb", sizeof(cfg->password) - 1);
    }

    if (nvs_get_u32(handle, "baud", &cfg->baud_rate) != ESP_OK ||
        cfg->baud_rate < 1200 || cfg->baud_rate > 230400)
        cfg->baud_rate = 9600;

    uint8_t otg_en = 0;
    if (nvs_get_u8(handle, "otg_en", &otg_en) != ESP_OK)
        cfg->otg_enabled = false;
    else
        cfg->otg_enabled = (bool)otg_en;

    /* ---- AP-Fallback ---- */
    len = sizeof(cfg->ap_ssid);
    if (nvs_get_str(handle, "ap_ssid", cfg->ap_ssid, &len) != ESP_OK)
        strncpy(cfg->ap_ssid, "ESP32S3_AP", sizeof(cfg->ap_ssid) - 1);

    len = sizeof(cfg->ap_pass);
    if (nvs_get_str(handle, "ap_pass", cfg->ap_pass, &len) != ESP_OK)
        strncpy(cfg->ap_pass, AP_PASS_DEFAULT, sizeof(cfg->ap_pass) - 1);
    config_normalize_ap_pass(cfg->ap_pass, sizeof(cfg->ap_pass));

    /* ---- MAC-Adressen ---- */
    len = sizeof(cfg->wifi_mac);
    if (nvs_get_str(handle, "wifi_mac", cfg->wifi_mac, &len) != ESP_OK)
        cfg->wifi_mac[0] = '\0';

    len = sizeof(cfg->eth_mac);
    if (nvs_get_str(handle, "eth_mac", cfg->eth_mac, &len) != ESP_OK)
        cfg->eth_mac[0] = '\0';

    /* ---- WireGuard ---- */
    uint8_t wg_en = 0;
    nvs_get_u8(handle, "wg_en", &wg_en);
    cfg->wg_enabled = (bool)wg_en;

    len = sizeof(cfg->wg_privkey);
    if (nvs_get_str(handle, "wg_priv", cfg->wg_privkey, &len) != ESP_OK)
        cfg->wg_privkey[0] = '\0';

    len = sizeof(cfg->wg_peer_pubkey);
    if (nvs_get_str(handle, "wg_peerpub", cfg->wg_peer_pubkey, &len) != ESP_OK)
        cfg->wg_peer_pubkey[0] = '\0';

    len = sizeof(cfg->wg_endpoint);
    if (nvs_get_str(handle, "wg_ep", cfg->wg_endpoint, &len) != ESP_OK)
        cfg->wg_endpoint[0] = '\0';

    if (nvs_get_u16(handle, "wg_port", &cfg->wg_port) != ESP_OK || cfg->wg_port == 0)
        cfg->wg_port = 51820;

    len = sizeof(cfg->wg_local_ip);
    if (nvs_get_str(handle, "wg_lclip", cfg->wg_local_ip, &len) != ESP_OK)
        strncpy(cfg->wg_local_ip, "10.8.0.2", sizeof(cfg->wg_local_ip) - 1);

    len = sizeof(cfg->wg_local_mask);
    if (nvs_get_str(handle, "wg_lclmask", cfg->wg_local_mask, &len) != ESP_OK)
        strncpy(cfg->wg_local_mask, "255.255.255.0", sizeof(cfg->wg_local_mask) - 1);

    if (nvs_get_u8(handle, "wg_ka", &cfg->wg_keepalive) != ESP_OK || cfg->wg_keepalive == 0)
        cfg->wg_keepalive = 25;

    nvs_close(handle);
    memcpy(&current_config, cfg, sizeof(app_config_t));
    ESP_LOGI(TAG, "Config geladen: SSID=%s Baud=%lu WG=%s OTG=%s",
             cfg->ssid, (unsigned long)cfg->baud_rate,
             cfg->wg_enabled ? "aktiv" : "deaktiviert",
             cfg->otg_enabled ? "aktiv" : "deaktiviert");
    return ESP_OK;
}

esp_err_t config_save(const app_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    /* WiFi STA */
    nvs_set_str(handle, "ssid",     cfg->ssid);
    nvs_set_str(handle, "password", cfg->password);
    nvs_set_u32(handle, "baud",     cfg->baud_rate);
    nvs_set_u8( handle, "otg_en",   cfg->otg_enabled ? 1 : 0);

    /* AP-Fallback */
    nvs_set_str(handle, "ap_ssid", cfg->ap_ssid[0] ? cfg->ap_ssid : "ESP32S3_AP");
    char ap_pass_save[65];
    strlcpy(ap_pass_save, cfg->ap_pass[0] ? cfg->ap_pass : AP_PASS_DEFAULT, sizeof(ap_pass_save));
    config_normalize_ap_pass(ap_pass_save, sizeof(ap_pass_save));
    nvs_set_str(handle, "ap_pass", ap_pass_save);

    /* MAC-Adressen */
    nvs_set_str(handle, "wifi_mac", cfg->wifi_mac);
    nvs_set_str(handle, "eth_mac",  cfg->eth_mac);

    /* WireGuard */
    nvs_set_u8( handle, "wg_en",      cfg->wg_enabled ? 1 : 0);
    nvs_set_str(handle, "wg_priv",    cfg->wg_privkey);
    nvs_set_str(handle, "wg_peerpub", cfg->wg_peer_pubkey);
    nvs_set_str(handle, "wg_ep",      cfg->wg_endpoint);
    nvs_set_u16(handle, "wg_port",    cfg->wg_port ? cfg->wg_port : 51820);
    nvs_set_str(handle, "wg_lclip",   cfg->wg_local_ip);
    nvs_set_str(handle, "wg_lclmask", cfg->wg_local_mask);
    nvs_set_u8( handle, "wg_ka",      cfg->wg_keepalive ? cfg->wg_keepalive : 25);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        memcpy(&current_config, cfg, sizeof(app_config_t));
    }
    ESP_LOGI(TAG, "Config gespeichert: SSID=%s WG=%s",
             cfg->ssid, cfg->wg_enabled ? "aktiv" : "deaktiviert");
    return err;
}

const app_config_t* config_get_current(void)
{
    return &current_config;
}
