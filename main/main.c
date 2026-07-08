/*
 * ESP32-S3 Cisco Out-of-Band Web Console
 *
 * Serielle Konsole ausschließlich über den nativen USB-C OTG-Port (USB Host).
 * Kein UART/GPIO — Cisco Switch direkt per USB-Kabel am ESP anschließen.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_sleep.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

#include "esp_eth.h"
#include "esp_mac.h"

#include "config.h"
#include "eth_w5500.h"
#include "wg_client.h"

static const char *TAG = "cisco_oob";

#define MAX_WS_CLIENTS   4
#define CDC_RX_BUF_SIZE  512
#define USB_HOST_START_DELAY_MS  3000

#define WIFI_MAX_RETRIES  8

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
static esp_err_t wifi_apply_ap_config(const app_config_t *cfg);

/* ===================== State ===================== */

static httpd_handle_t server = NULL;
static int  ws_clients[MAX_WS_CLIENTS];
static SemaphoreHandle_t ws_mutex  = NULL;

static cdc_acm_dev_hdl_t cdc_dev   = NULL;
static SemaphoreHandle_t cdc_mutex = NULL;
static uint32_t current_baud       = 9600;
static bool cdc_connected          = false;

/* Unterdrückt USB-TX-Echo (gesendete Zeichen kommen oft nochmal auf RX zurück).
 * Ring-Buffer: O(1) pro Byte statt O(n) memmove. */
#define TX_ECHO_BUF_SIZE  512
static uint8_t  tx_echo_buf[TX_ECHO_BUF_SIZE];
static size_t   tx_echo_head  = 0;   /* Lese-Index */
static size_t   tx_echo_tail  = 0;   /* Schreib-Index */
static size_t   tx_echo_count = 0;
static SemaphoreHandle_t tx_echo_mutex = NULL;

static size_t filter_tx_echo(uint8_t *buf, size_t len)
{
    size_t r = 0, w = 0;
    if (!tx_echo_mutex || len == 0) return len;

    xSemaphoreTake(tx_echo_mutex, portMAX_DELAY);
    while (r < len) {
        if (tx_echo_count > 0 && buf[r] == tx_echo_buf[tx_echo_head]) {
            tx_echo_head = (tx_echo_head + 1) % TX_ECHO_BUF_SIZE;
            tx_echo_count--;
            r++;
        } else {
            buf[w++] = buf[r++];
        }
    }
    xSemaphoreGive(tx_echo_mutex);
    return w;
}

static void tx_echo_push(const uint8_t *data, size_t len)
{
    if (!tx_echo_mutex || !data || len == 0) return;

    xSemaphoreTake(tx_echo_mutex, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) {
        if (tx_echo_count == TX_ECHO_BUF_SIZE) {
            /* Ältestes Byte verwerfen */
            tx_echo_head = (tx_echo_head + 1) % TX_ECHO_BUF_SIZE;
            tx_echo_count--;
        }
        tx_echo_buf[tx_echo_tail] = data[i];
        tx_echo_tail = (tx_echo_tail + 1) % TX_ECHO_BUF_SIZE;
        tx_echo_count++;
    }
    xSemaphoreGive(tx_echo_mutex);
}

/* ===================== Forward decls ===================== */
static void remove_ws_client(int fd);
static void broadcast_to_ws(const uint8_t *data, size_t len);
static httpd_handle_t start_webserver(void);
static void reboot_timer_cb(void *arg);
static void shutdown_timer_cb(void *arg);
static esp_err_t reboot_handler(httpd_req_t *req);
static esp_err_t shutdown_handler(httpd_req_t *req);
static esp_err_t ws_post_handshake_cb(httpd_req_t *req);

/* ===================== WebSocket client management ===================== */

static void init_ws_clients(void)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) ws_clients[i] = -1;
    ws_mutex = xSemaphoreCreateMutex();
}

static void add_ws_client(int fd)
{
    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    int empty = -1;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i] == fd) { xSemaphoreGive(ws_mutex); return; }
        if (ws_clients[i] < 0 && empty < 0) empty = i;
    }
    if (empty >= 0) {
        ws_clients[empty] = fd;
        ESP_LOGI(TAG, "WS client fd=%d added", fd);
    }
    xSemaphoreGive(ws_mutex);
}

static void remove_ws_client(int fd)
{
    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i] == fd) { ws_clients[i] = -1; break; }
    }
    xSemaphoreGive(ws_mutex);
    ESP_LOGI(TAG, "WS client fd=%d removed", fd);
}

static void broadcast_to_ws(const uint8_t *data, size_t len)
{
    if (!server || !ws_mutex || len == 0) return;

    httpd_ws_frame_t pkt = {
        .payload = (uint8_t *)data,
        .len     = len,
        .type    = HTTPD_WS_TYPE_BINARY,
    };

    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i] >= 0) {
            esp_err_t ret = httpd_ws_send_frame_async(server, ws_clients[i], &pkt);
            if (ret != ESP_OK) {
                ESP_LOGD(TAG, "WS send failed fd=%d, dropping", ws_clients[i]);
                ws_clients[i] = -1;
            }
        }
    }
    xSemaphoreGive(ws_mutex);
}

typedef struct {
    uint8_t *data;
    size_t   len;
} ws_broadcast_job_t;

static void ws_broadcast_work(void *arg)
{
    ws_broadcast_job_t *job = arg;
    if (job) {
        broadcast_to_ws(job->data, job->len);
        free(job->data);
        free(job);
    }
}

static void queue_broadcast_to_ws(const uint8_t *data, size_t len)
{
    if (!server || !data || len == 0) return;

    ws_broadcast_job_t *job = calloc(1, sizeof(*job));
    if (!job) return;

    job->data = malloc(len);
    if (!job->data) {
        free(job);
        return;
    }
    memcpy(job->data, data, len);
    job->len = len;

    if (httpd_queue_work(server, ws_broadcast_work, job) != ESP_OK) {
        free(job->data);
        free(job);
    }
}

/* ===================== CDC-ACM callbacks ===================== */

static bool cdc_rx_callback(const uint8_t *data, size_t data_len, void *arg)
{
    if (data_len == 0) return true;

    /* Direkt Job-Puffer befüllen — eine Allokation + eine Kopie statt zwei. */
    ws_broadcast_job_t *job = calloc(1, sizeof(*job));
    if (!job) return true;

    job->data = malloc(data_len);
    if (!job->data) { free(job); return true; }

    memcpy(job->data, data, data_len);
    job->len = filter_tx_echo(job->data, data_len);

    if (job->len == 0 || httpd_queue_work(server, ws_broadcast_work, job) != ESP_OK) {
        free(job->data);
        free(job);
    }
    return true;
}

static void cdc_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "CDC device disconnected — handle=%p", event->data.cdc_hdl);
        cdc_connected = false;
        xSemaphoreTake(cdc_mutex, portMAX_DELAY);
        cdc_dev = NULL;
        xSemaphoreGive(cdc_mutex);
        /* Close the handle returned in the event */
        cdc_acm_host_close(event->data.cdc_hdl);
        {
            const char *msg = "\r\n[ESP] Cisco disconnected\r\n";
            queue_broadcast_to_ws((const uint8_t *)msg, strlen(msg));
        }
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC error code: %d", event->data.error);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "CDC serial state change");
        break;
    default:
        break;
    }
}

/* ===================== USB Host task ===================== */

static void usb_host_task(void *arg)
{
    ESP_LOGI(TAG, "USB host task started");
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "USB: no clients");
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: all freed");
        }
    }
}

static void cdc_on_connected(cdc_acm_dev_hdl_t new_dev)
{
    cdc_connected = true;

    cdc_acm_line_coding_t line_coding = {
        .dwDTERate   = current_baud,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits   = 8,
    };
    cdc_acm_host_line_coding_set(new_dev, &line_coding);
    /* DTR+RTS: Cisco-Konsole braucht oft beides für TX vom Switch */
    cdc_acm_host_set_control_line_state(new_dev, true, true);

    xSemaphoreTake(cdc_mutex, portMAX_DELAY);
    cdc_dev = new_dev;
    xSemaphoreGive(cdc_mutex);

    ESP_LOGI(TAG, "CDC opened @ %lu baud — device ready", (unsigned long)current_baud);
    cdc_acm_host_desc_print(new_dev);

    const char *conn_msg = "\r\n[ESP] Cisco verbunden (USB-OTG)\r\n";
    queue_broadcast_to_ws((const uint8_t *)conn_msg, strlen(conn_msg));

    /* Konsole aufwecken */
    const uint8_t wake[] = "\r\n";
    vTaskDelay(pdMS_TO_TICKS(100));
    cdc_acm_host_data_tx_blocking(new_dev, wake, sizeof(wake) - 1, 500);
}

static void cdc_connect_task(void *arg)
{
    /* Install CDC-ACM driver */
    const cdc_acm_host_driver_config_t driver_cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority   = 10,
        .xCoreID                = 0,
        .new_dev_cb             = NULL,
    };
    ESP_ERROR_CHECK(cdc_acm_host_install(&driver_cfg));

    ESP_LOGI(TAG, "CDC connect task started, waiting for USB device...");
    while (1) {
        if (cdc_dev == NULL) {
            ESP_LOGI(TAG, "CDC: trying to open device (ANY VID/PID, timeout 5s)...");
            /* Open any CDC-ACM device (Cisco USB console = VID 0x05f9 PID 0x4004,
               aber wir nutzen CDC_HOST_ANY_VID/PID um jeden Adapter zu akzeptieren) */
            const cdc_acm_host_device_config_t dev_cfg = {
                .connection_timeout_ms = 5000,
                .out_buffer_size       = 512,
                .in_buffer_size        = CDC_RX_BUF_SIZE,
                .event_cb              = cdc_event_callback,
                .data_cb               = cdc_rx_callback,
                .user_arg              = NULL,
            };

            cdc_acm_dev_hdl_t new_dev = NULL;
            esp_err_t ret = cdc_acm_host_open(
                CDC_HOST_ANY_VID, CDC_HOST_ANY_PID,
                0,       /* interface number */
                &dev_cfg,
                &new_dev
            );

            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "CDC open failed: %s — retrying in 2s", esp_err_to_name(ret));
            } else if (new_dev) {
                cdc_on_connected(new_dev);
            }
        } else {
            ESP_LOGD(TAG, "CDC: device still connected");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void usb_host_init(void)
{
    cdc_mutex = xSemaphoreCreateMutex();
    if (!tx_echo_mutex) {
        tx_echo_mutex = xSemaphoreCreateMutex();
    }

    const usb_host_config_t host_cfg = {
        .skip_phy_setup    = false,
        .intr_flags        = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    xTaskCreate(usb_host_task,   "usb_host",   4096, NULL, 12, NULL);
    xTaskCreate(cdc_connect_task, "cdc_conn",  4096, NULL, 10, NULL);
}

/* USB-Host verzögert starten: Boot-Logs zuerst über USB-Serial/JTAG sichtbar */
static void usb_host_delayed_task(void *arg)
{
    ESP_LOGI(TAG, "USB-Host startet in %d ms (Boot-Log über USB-C nutzen)", USB_HOST_START_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(USB_HOST_START_DELAY_MS));
    ESP_LOGI(TAG, "OTG mode — starting USB Host");
    usb_host_init();
    vTaskDelete(NULL);
}

static void usb_host_init_delayed(void)
{
    xTaskCreate(usb_host_delayed_task, "usb_delay", 3072, NULL, 5, NULL);
}

static esp_err_t cdc_send(const uint8_t *data, size_t len)
{
    xSemaphoreTake(cdc_mutex, portMAX_DELAY);
    cdc_acm_dev_hdl_t dev = cdc_dev;
    xSemaphoreGive(cdc_mutex);

    if (!dev) {
        ESP_LOGW(TAG, "cdc_send: no device connected");
        return ESP_ERR_INVALID_STATE;
    }
    tx_echo_push(data, len);
    ESP_LOGD(TAG, "CDC TX %u bytes", (unsigned)len);
    esp_err_t ret = cdc_acm_host_data_tx_blocking(dev, data, len, 500);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CDC TX failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void send_break(void)
{
    xSemaphoreTake(cdc_mutex, portMAX_DELAY);
    cdc_acm_dev_hdl_t dev = cdc_dev;
    xSemaphoreGive(cdc_mutex);

    if (!dev) return;
    ESP_LOGI(TAG, "Sending BREAK");
    cdc_acm_host_send_break(dev, 250);  /* 250ms break */
}

static void set_baud(uint32_t baud)
{
    current_baud = baud;

    xSemaphoreTake(cdc_mutex, portMAX_DELAY);
    cdc_acm_dev_hdl_t dev = cdc_dev;
    xSemaphoreGive(cdc_mutex);

    if (!dev) return;

    cdc_acm_line_coding_t lc = {
        .dwDTERate   = baud,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits   = 8,
    };
    cdc_acm_host_line_coding_set(dev, &lc);
    ESP_LOGI(TAG, "Baud set to %lu", (unsigned long)baud);
}

/* ===================== WiFi ===================== */

static bool wg_started    = false;
static bool sntp_started  = false;
static bool ap_mode       = false;
static bool otg_enabled   = false;   /* true nur wenn USB-Host-Code aktiv */
static int  wifi_retries  = 0;
static bool eth_connected = false;
static bool wifi_sta_off  = false;   /* true wenn STA wegen ETH-IP gestoppt */

static void netif_apply_mtu(const char *ifkey)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey(ifkey);
    if (!n) return;
    esp_err_t e = esp_netif_set_mtu(n, NETIF_MTU);
    if (e == ESP_OK)
        ESP_LOGI(TAG, "MTU %d: %s", NETIF_MTU, ifkey);
    else
        ESP_LOGW(TAG, "MTU %s: %s", ifkey, esp_err_to_name(e));
}

static bool eth_has_active_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(netif, &ip);
    return ip.ip.addr != 0;
}

/* ETH hat DHCP-IP → WLAN-STA komplett abschalten (Strom sparen, kein Dual-Radio) */
static void wifi_sta_shutdown(void)
{
    if (ap_mode || wifi_sta_off) return;
    wifi_sta_off = true;
    wifi_retries = 0;
    ESP_LOGI(TAG, "ETH mit IP aktiv — WLAN STA wird ausgeschaltet");
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_stop();
}

/* ETH-IP/Link weg → WLAN-STA wieder starten */
static void wifi_sta_restore(void)
{
    if (ap_mode || !wifi_sta_off) return;
    if (eth_has_active_ip()) return;
    wifi_sta_off = false;
    wifi_retries = 0;
    ESP_LOGI(TAG, "ETH inaktiv — WLAN STA wird wieder aktiviert");
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK)
        ESP_LOGW(TAG, "WLAN-Start fehlgeschlagen: %s", esp_err_to_name(e));
}

/* WireGuard neu starten wenn Underlay-Interface wechselt (ETH ↔ WiFi). */
static void wg_client_restart_if_running(void)
{
    if (!wg_started) return;
    const app_config_t *cfg = config_get_current();
    if (!cfg->wg_enabled) return;

    ESP_LOGI(TAG, "WireGuard-Neustart wegen Schnittstellenwechsel");
    wg_client_stop();
    esp_err_t ret = wg_client_start();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "WireGuard Neustart: %s", esp_err_to_name(ret));
    }
}

static void start_ap_mode(void)
{
    ap_mode = true;
    const app_config_t *cfg = config_get_current();
    const char *ssid = cfg->ap_ssid[0] ? cfg->ap_ssid : "ESP32S3_AP";
    ESP_LOGW(TAG, "WiFi nicht erreichbar — starte Fallback-AP: SSID=%s (Passwort: %u Zeichen)",
             ssid, (unsigned)strlen(cfg->ap_pass));

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_AP);

    esp_err_t err = wifi_apply_ap_config(cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP-Config fehlgeschlagen: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    /* Webserver startet im WIFI_EVENT_AP_START Handler */
}

static esp_err_t wifi_apply_ap_config(const app_config_t *cfg)
{
    char ssid[33];
    char pass[65];

    strlcpy(ssid, cfg->ap_ssid[0] ? cfg->ap_ssid : "ESP32S3_AP", sizeof(ssid));
    strlcpy(pass, cfg->ap_pass[0] ? cfg->ap_pass : AP_PASS_DEFAULT, sizeof(pass));
    config_normalize_ap_pass(pass, sizeof(pass));

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(ssid);
    ap_cfg.ap.channel        = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.pmf_cfg.capable  = true;
    ap_cfg.ap.pmf_cfg.required = false;
#if CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
#else
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
#endif

    return esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

static void wifi_stack_init_common(void)
{
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    netif_apply_mtu("WIFI_STA_DEF");
    netif_apply_mtu("WIFI_AP_DEF");

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,              &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,           &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_ETH_GOT_IP,           &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_ETH_LOST_IP,          &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(ETH_EVENT,  ETHERNET_EVENT_CONNECTED,      &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(ETH_EVENT,  ETHERNET_EVENT_DISCONNECTED,   &wifi_event_handler, NULL, NULL);
}

static void wifi_start_ap_direct(void)
{
    const app_config_t *cfg = config_get_current();
    wifi_stack_init_common();
    ESP_ERROR_CHECK(wifi_apply_ap_config(cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_start());
    ap_mode = true;
    ESP_LOGI(TAG, "Direktstart Fallback-AP: SSID=%s", cfg->ap_ssid[0] ? cfg->ap_ssid : "ESP32S3_AP");
}

/* Startet WireGuard erst nachdem NTP die Systemzeit synchronisiert hat.
 * WireGuard-Handshakes enthalten einen TAI64N-Timestamp; ohne korrekte
 * Uhrzeit verwirft der Server die Initiation-Pakete (Replay-Schutz).   */
static void wg_start_after_ntp_task(void *arg)
{
    int retries = 0;
    while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retries < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retries++;
    }

    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        time_t now;
        time(&now);
        struct tm t;
        gmtime_r(&now, &t);
        ESP_LOGI(TAG, "NTP sync: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        ESP_LOGW(TAG, "NTP-Timeout nach 10 s — WireGuard startet trotzdem");
    }

    if (!wg_started) {
        wg_started = true;
        esp_err_t ret = wg_client_start();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "WireGuard start: %s", esp_err_to_name(ret));
        }
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!wifi_sta_off)
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_sta_off || eth_connected) return;
        if (!ap_mode) {
            wifi_retries++;
            if (wifi_retries <= WIFI_MAX_RETRIES) {
                ESP_LOGW(TAG, "WiFi disconnected — Retry %d/%d", wifi_retries, WIFI_MAX_RETRIES);
                esp_wifi_connect();
            } else if (!eth_has_active_ip()) {
                start_ap_mode();
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        const app_config_t *ap_cfg = config_get_current();
        ESP_LOGI(TAG, "AP aktiv — SSID: %s  IP: 192.168.4.1",
                 ap_cfg->ap_ssid[0] ? ap_cfg->ap_ssid : "ESP32S3_AP");
        if (!server) start_webserver();
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        wifi_retries = 0;
        eth_connected = true;
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Ethernet (W5500) — IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        esp_netif_t *eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        if (eth_netif) esp_netif_set_default_netif(eth_netif);
        if (ev->ip_info.ip.addr != 0)
            wifi_sta_shutdown();
        wg_client_restart_if_running();
        if (!server) start_webserver();
        if (!sntp_started) {
            sntp_started = true;
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_setservername(1, "time.cloudflare.com");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP gestartet — WireGuard startet nach Zeitsync");
            xTaskCreate(wg_start_after_ntp_task, "wg_ntp", 2560, NULL, 4, NULL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_retries = 0;
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi — IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        /* WiFi nur als Standard wenn kein ETH aktiv */
        if (!eth_connected) {
            esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (wifi_netif) esp_netif_set_default_netif(wifi_netif);
        }
        wg_client_restart_if_running();
        if (!server) start_webserver();
        if (!sntp_started) {
            sntp_started = true;
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_setservername(1, "time.cloudflare.com");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP gestartet — WireGuard startet nach Zeitsync");
            xTaskCreate(wg_start_after_ntp_task, "wg_ntp", 2560, NULL, 4, NULL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        if (eth_connected) {
            eth_connected = false;
            ESP_LOGW(TAG, "ETH-IP verloren (DHCP) — WLAN wird reaktiviert");
            wifi_sta_restore();
        }
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        if (eth_connected) {
            eth_connected = false;
            ESP_LOGW(TAG, "ETH Link Down — WLAN wird reaktiviert");
            wifi_sta_restore();
        }
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "ETH Link Up — warte auf DHCP-IP");
    }
}

static void wifi_init_sta(void)
{
    const app_config_t *cfg = config_get_current();
    wifi_stack_init_common();

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid,     cfg->ssid,     sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, cfg->password, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(wifi_apply_ap_config(cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    /* Benutzerdefinierte WLAN-MAC setzen (muss vor esp_wifi_start erfolgen) */
    if (cfg->wifi_mac[0] != '\0') {
        uint8_t mac[6];
        unsigned int m[6] = {0};
        if (sscanf(cfg->wifi_mac, "%x:%x:%x:%x:%x:%x",
                   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
            esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, mac);
            if (e == ESP_OK)
                ESP_LOGI(TAG, "WLAN-MAC gesetzt: %02x:%02x:%02x:%02x:%02x:%02x",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            else
                ESP_LOGW(TAG, "WLAN-MAC setzen fehlgeschlagen: %s", esp_err_to_name(e));
        } else {
            ESP_LOGW(TAG, "Ungültige WLAN-MAC '%s' — ignoriert", cfg->wifi_mac);
        }
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi STA connecting to %s", cfg->ssid);
}

static void wifi_reconnect(const char *ssid, const char *pass)
{
    if (wifi_sta_off || eth_connected) {
        ESP_LOGI(TAG, "WLAN-Reconnect übersprungen — ETH aktiv bzw. STA aus");
        return;
    }
    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid,     ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_connect();
}

/* ===================== HTTP handlers ===================== */

extern const char web_terminal_html_start[] asm("_binary_web_terminal_html_start");
extern const char web_terminal_html_end[]   asm("_binary_web_terminal_html_end");

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, web_terminal_html_start,
                    web_terminal_html_end - web_terminal_html_start);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char buf[384];
    esp_netif_ip_info_t wifi_ip = {0}, eth_ip = {0}, active_ip = {0};

    esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (wifi_netif) esp_netif_get_ip_info(wifi_netif, &wifi_ip);

    esp_netif_t *eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (eth_netif) esp_netif_get_ip_info(eth_netif, &eth_ip);

    /* Aktive (Standard-)Schnittstelle: ETH bevorzugt wenn verbunden */
    esp_netif_t *def_netif = esp_netif_get_default_netif();
    if (def_netif) esp_netif_get_ip_info(def_netif, &active_ip);

    snprintf(buf, sizeof(buf),
             "{\"ip\":\"" IPSTR "\",\"eth_ip\":\"" IPSTR "\","
             "\"active_ip\":\"" IPSTR "\","
             "\"wg_ip\":\"%s\",\"wg_up\":%s,"
             "\"baud\":%lu,\"usb\":%s,\"ap\":%s,\"otg_en\":%s}",
             IP2STR(&wifi_ip.ip),
             IP2STR(&eth_ip.ip),
             IP2STR(&active_ip.ip),
             wg_client_local_ip(),
             wg_client_is_up() ? "true" : "false",
             (unsigned long)current_baud,
             cdc_connected ? "true" : "false",
             ap_mode ? "true" : "false",
             otg_enabled ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t break_post_handler(httpd_req_t *req)
{
    send_break();
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t baud_post_handler(httpd_req_t *req)
{
    char buf[32] = {0};
    int r = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (r <= 0) return ESP_FAIL;
    buf[r] = 0;

    uint32_t baud = (uint32_t)atoi(buf);
    if (baud < 1200 || baud > 230400) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad baud");
        return ESP_FAIL;
    }
    set_baud(baud);

    app_config_t cfg = *config_get_current();
    cfg.baud_rate = baud;
    config_save(&cfg);

    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t config_json_get_handler(httpd_req_t *req)
{
    const app_config_t *cfg = config_get_current();

    /* Aktuelle WLAN-MAC (nach optionalem set_mac) */
    uint8_t wm[6] = {0};
    char wifi_mac_cur[18] = "";
    if (esp_wifi_get_mac(WIFI_IF_STA, wm) == ESP_OK)
        snprintf(wifi_mac_cur, sizeof(wifi_mac_cur),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 wm[0], wm[1], wm[2], wm[3], wm[4], wm[5]);

    /* Auto-ETH-MAC aus eFuse für Placeholder */
    uint8_t em[6] = {0};
    char eth_mac_auto[18] = "";
    if (esp_read_mac(em, ESP_MAC_ETH) == ESP_OK)
        snprintf(eth_mac_auto, sizeof(eth_mac_auto),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 em[0], em[1], em[2], em[3], em[4], em[5]);

    char buf[448];
    snprintf(buf, sizeof(buf),
             "{\"ssid\":\"%s\",\"baud\":%lu,\"otg_enabled\":%s,"
             "\"ap_ssid\":\"%s\","
             "\"wifi_mac\":\"%s\",\"wifi_mac_auto\":\"%s\","
             "\"eth_mac\":\"%s\",\"eth_mac_auto\":\"%s\"}",
             cfg->ssid,
             (unsigned long)cfg->baud_rate,
             cfg->otg_enabled ? "true" : "false",
             cfg->ap_ssid[0] ? cfg->ap_ssid : "ESP32S3_AP",
             cfg->wifi_mac,
             wifi_mac_cur,
             cfg->eth_mac,
             eth_mac_auto);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* Dekodiert URL-Encoding (+ → Leerzeichen, %XX → Byte) in-place. */
static void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' '; r++;
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = {r[1], r[2], '\0'};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Extrahiert den Wert eines URL-encoded Formular-Feldes (key=) nach dst (max dst_size-1 Zeichen). */
static void form_field(const char *body, const char *key, char *dst, size_t dst_size)
{
    char *p = strstr(body, key);
    if (!p) { dst[0] = '\0'; return; }
    p += strlen(key);
    char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, p, len);
    dst[len] = '\0';
    url_decode(dst);
}

/* Prüft ob s das Format "XX:XX:XX:XX:XX:XX" hat */
static bool mac_str_valid(const char *s)
{
    if (!s || strlen(s) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (s[i] != ':') return false; }
        else             { if (!isxdigit((unsigned char)s[i])) return false; }
    }
    return true;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[768] = {0};
    int total = 0, r;
    while ((r = httpd_req_recv(req, buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += r;
        if (total >= (int)sizeof(buf) - 1) break;
    }
    buf[total] = '\0';

    app_config_t new_cfg = *config_get_current();   /* alle Felder erhalten */

    /* Merken ob MAC, OTG oder AP-Daten sich ändert */
    char old_wifi_mac[18], old_eth_mac[18];
    char old_ap_ssid[33], old_ap_pass[65];
    bool old_otg_enabled = new_cfg.otg_enabled;
    strlcpy(old_wifi_mac, new_cfg.wifi_mac, sizeof(old_wifi_mac));
    strlcpy(old_eth_mac,  new_cfg.eth_mac,  sizeof(old_eth_mac));
    strlcpy(old_ap_ssid,  new_cfg.ap_ssid,  sizeof(old_ap_ssid));
    strlcpy(old_ap_pass,  new_cfg.ap_pass,  sizeof(old_ap_pass));

    form_field(buf, "ssid=", new_cfg.ssid, sizeof(new_cfg.ssid));

    char password_tmp[65] = {0};
    form_field(buf, "password=", password_tmp, sizeof(password_tmp));
    if (password_tmp[0]) strlcpy(new_cfg.password, password_tmp, sizeof(new_cfg.password));

    char baud_str[16];
    form_field(buf, "baud=", baud_str, sizeof(baud_str));
    new_cfg.baud_rate = baud_str[0] ? (uint32_t)atoi(baud_str) : 9600;

    char otg_str[8] = {0};
    form_field(buf, "otg_en=", otg_str, sizeof(otg_str));
    if (otg_str[0])
        new_cfg.otg_enabled = (otg_str[0] == '1');

    /* AP-Fallback */
    char ap_ssid_tmp[33] = {0};
    char ap_pass_tmp[65] = {0};
    form_field(buf, "ap_ssid=", ap_ssid_tmp, sizeof(ap_ssid_tmp));
    form_field(buf, "ap_pass=", ap_pass_tmp,  sizeof(ap_pass_tmp));
    if (ap_ssid_tmp[0]) strlcpy(new_cfg.ap_ssid, ap_ssid_tmp, sizeof(new_cfg.ap_ssid));
    if (ap_pass_tmp[0]) {
        if (!config_ap_pass_valid(ap_pass_tmp)) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ap_pass_too_short\"}");
            return ESP_OK;
        }
        strlcpy(new_cfg.ap_pass, ap_pass_tmp, sizeof(new_cfg.ap_pass));
    }
    if (new_cfg.ap_ssid[0] == '\0') strlcpy(new_cfg.ap_ssid, "ESP32S3_AP", sizeof(new_cfg.ap_ssid));
    config_normalize_ap_pass(new_cfg.ap_pass, sizeof(new_cfg.ap_pass));

    /* MAC-Adressen */
    char wifi_mac_tmp[18] = {0};
    char eth_mac_tmp[18]  = {0};
    form_field(buf, "wifi_mac=", wifi_mac_tmp, sizeof(wifi_mac_tmp));
    form_field(buf, "eth_mac=",  eth_mac_tmp,  sizeof(eth_mac_tmp));
    /* Leer = Auto (eFuse), ungültig = ignorieren */
    if (wifi_mac_tmp[0] == '\0')
        new_cfg.wifi_mac[0] = '\0';
    else if (mac_str_valid(wifi_mac_tmp))
        strlcpy(new_cfg.wifi_mac, wifi_mac_tmp, sizeof(new_cfg.wifi_mac));
    if (eth_mac_tmp[0] == '\0')
        new_cfg.eth_mac[0] = '\0';
    else if (mac_str_valid(eth_mac_tmp))
        strlcpy(new_cfg.eth_mac, eth_mac_tmp, sizeof(new_cfg.eth_mac));

    bool mac_changed = (strcmp(old_wifi_mac, new_cfg.wifi_mac) != 0) ||
                       (strcmp(old_eth_mac,  new_cfg.eth_mac)  != 0);
    bool otg_changed = (old_otg_enabled != new_cfg.otg_enabled);
    bool ap_changed  = (strcmp(old_ap_ssid, new_cfg.ap_ssid) != 0) ||
                       (strcmp(old_ap_pass,  new_cfg.ap_pass)  != 0);

    if (new_cfg.ssid[0] == '\0') strlcpy(new_cfg.ssid, "net1", sizeof(new_cfg.ssid));

    config_save(&new_cfg);
    set_baud(new_cfg.baud_rate);

    if (ap_mode)
        config_set_boot_ap(true);

    if (!ap_mode && ap_changed)
        wifi_apply_ap_config(&new_cfg);

    if (ap_mode || mac_changed || otg_changed) {
        /* Reboot erforderlich: AP-Modus aktiv, MAC- oder OTG-Änderung */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
        esp_timer_handle_t t;
        esp_timer_create(&(esp_timer_create_args_t){
            .callback = reboot_timer_cb, .name = "cfg_reboot"}, &t);
        esp_timer_start_once(t, 2000000);
        return ESP_OK;
    }

    wifi_reconnect(new_cfg.ssid, new_cfg.password);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ===================== WireGuard HTTP-Handler ===================== */

static esp_err_t wg_json_get_handler(httpd_req_t *req)
{
    const app_config_t *cfg = config_get_current();
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"enabled\":%s,\"peer_pubkey\":\"%s\",\"endpoint\":\"%s\","
             "\"port\":%d,\"local_ip\":\"%s\",\"local_mask\":\"%s\","
             "\"keepalive\":%d,\"up\":%s,\"has_privkey\":%s}",
             cfg->wg_enabled ? "true" : "false",
             cfg->wg_peer_pubkey,
             cfg->wg_endpoint,
             cfg->wg_port ? cfg->wg_port : 51820,
             cfg->wg_local_ip[0]   ? cfg->wg_local_ip   : "10.8.0.3",
             cfg->wg_local_mask[0] ? cfg->wg_local_mask : "255.255.255.0",
             cfg->wg_keepalive ? cfg->wg_keepalive : 25,
             wg_client_is_up() ? "true" : "false",
             cfg->wg_privkey[0]    ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}


static esp_err_t wg_post_handler(httpd_req_t *req)
{
    char buf[768] = {0};
    int total = 0, r;
    while ((r = httpd_req_recv(req, buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += r;
        if (total >= (int)sizeof(buf) - 1) break;
    }
    buf[total] = '\0';

    app_config_t new_cfg = *config_get_current();  /* alle bisherigen Werte übernehmen */

    char tmp[64] = {0};

    form_field(buf, "enabled=", tmp, sizeof(tmp));
    new_cfg.wg_enabled = (tmp[0] == '1');

    /* Privater Schlüssel: nur überschreiben, wenn neuer Wert eingegeben wurde */
    char new_priv[48] = {0};
    form_field(buf, "privkey=", new_priv, sizeof(new_priv));
    if (new_priv[0] != '\0') {
        strlcpy(new_cfg.wg_privkey, new_priv, sizeof(new_cfg.wg_privkey));
    }

    form_field(buf, "peer_pubkey=", new_cfg.wg_peer_pubkey, sizeof(new_cfg.wg_peer_pubkey));
    form_field(buf, "endpoint=",    new_cfg.wg_endpoint,    sizeof(new_cfg.wg_endpoint));

    memset(tmp, 0, sizeof(tmp));
    form_field(buf, "port=", tmp, sizeof(tmp));
    new_cfg.wg_port = tmp[0] ? (uint16_t)atoi(tmp) : 51820;

    form_field(buf, "local_ip=",   new_cfg.wg_local_ip,   sizeof(new_cfg.wg_local_ip));
    form_field(buf, "local_mask=", new_cfg.wg_local_mask, sizeof(new_cfg.wg_local_mask));

    memset(tmp, 0, sizeof(tmp));
    form_field(buf, "keepalive=", tmp, sizeof(tmp));
    new_cfg.wg_keepalive = tmp[0] ? (uint8_t)atoi(tmp) : 25;

    /* Standardwerte wenn leer */
    if (new_cfg.wg_local_ip[0]   == '\0') strlcpy(new_cfg.wg_local_ip,   "10.8.0.2",       sizeof(new_cfg.wg_local_ip));
    if (new_cfg.wg_local_mask[0] == '\0') strlcpy(new_cfg.wg_local_mask, "255.255.255.0",   sizeof(new_cfg.wg_local_mask));
    if (new_cfg.wg_port == 0)  new_cfg.wg_port = 51820;

    config_save(&new_cfg);

    /* WireGuard neu starten */
    wg_client_stop();
    wg_started = false;
    if (new_cfg.wg_enabled) {
        wg_started = true;
        esp_err_t ret = wg_client_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "WireGuard Neustart: %s", esp_err_to_name(ret));
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ================================================================== */

static void ws_send_status_to_client(int fd)
{
    (void)fd;
    char msg[192];
    snprintf(msg, sizeof(msg),
             "\r\n[ESP] Terminal bereit | USB: %s | Baud: %lu\r\n"
             "Tippen Sie Befehle ein (z.B. show version) + Enter\r\n",
             cdc_connected ? "verbunden" : "warte auf Geraet...",
             (unsigned long)current_baud);
    queue_broadcast_to_ws((const uint8_t *)msg, strlen(msg));
}

static esp_err_t ws_post_handshake_cb(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    add_ws_client(fd);
    ws_send_status_to_client(fd);
    ESP_LOGI(TAG, "WS handshake done, fd=%d", fd);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (httpd_ws_get_fd_info(req->handle, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        remove_ws_client(fd);
        return ret;
    }

    if (ws_pkt.len == 0) {
        return ESP_OK;
    }

    uint8_t *buf = calloc(1, ws_pkt.len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(buf);
        remove_ws_client(fd);
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        remove_ws_client(fd);
    } else if ((ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_BINARY) &&
               ws_pkt.len >= 9 && memcmp(buf, "__BREAK__", 9) == 0) {
        send_break();
    } else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
        if (cdc_send(buf, ws_pkt.len) != ESP_OK) {
            const char *err = "\r\n[ESP] USB nicht verbunden — Befehl nicht gesendet\r\n";
            queue_broadcast_to_ws((const uint8_t *)err, strlen(err));
        }
    }

    free(buf);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable  = true;
    config.max_open_sockets  = 4;
        config.max_uri_handlers  = 20;
    config.stack_size        = 10240;

    if (httpd_start(&server, &config) == ESP_OK) {
        init_ws_clients();

        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/", .method=HTTP_GET, .handler=root_get_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/favicon.ico", .method=HTTP_GET, .handler=favicon_get_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/ws", .method=HTTP_GET, .handler=ws_handler,
            .is_websocket=true, .ws_post_handshake_cb=ws_post_handshake_cb});
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/status", .method=HTTP_GET, .handler=status_get_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/break", .method=HTTP_POST, .handler=break_post_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/baud", .method=HTTP_POST, .handler=baud_post_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/config", .method=HTTP_POST, .handler=config_post_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/config-json", .method=HTTP_GET, .handler=config_json_get_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/reboot", .method=HTTP_POST, .handler=reboot_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/shutdown", .method=HTTP_POST, .handler=shutdown_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/wg", .method=HTTP_POST, .handler=wg_post_handler});
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri="/wg-json", .method=HTTP_GET, .handler=wg_json_get_handler});

        ESP_LOGI(TAG, "HTTP + WebSocket server started");
    }
    return server;
}

/* ===================== Reboot handler ===================== */

static void reboot_timer_cb(void *arg)
{
    esp_restart();
}

static void shutdown_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Shutdown — Deep Sleep (Reset zum Einschalten)");
    wg_client_stop();
    esp_wifi_stop();
    esp_deep_sleep_start();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif;margin:2em;background:#111;color:#ddd'>"
        "<h2>Rebooting...</h2><p>ESP32 wird neu gestartet.</p>"
        "<script>setTimeout(()=>location.href='/',5000)</script>"
        "</body></html>");
    const esp_timer_create_args_t ta = {.callback = reboot_timer_cb, .name = "reboot"};
    esp_timer_handle_t t;
    esp_timer_create(&ta, &t);
    esp_timer_start_once(t, 500 * 1000);
    return ESP_OK;
}

static esp_err_t shutdown_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif;margin:2em;background:#111;color:#ddd'>"
        "<h2>Shutdown</h2><p>ESP32 wird ausgeschaltet.</p>"
        "<p style='color:#888'>Zum Einschalten Reset-Taste drücken oder Strom kurz trennen.</p>"
        "</body></html>");
    const esp_timer_create_args_t ta = {.callback = shutdown_timer_cb, .name = "shutdown"};
    esp_timer_handle_t t;
    esp_timer_create(&ta, &t);
    esp_timer_start_once(t, 500 * 1000);
    return ESP_OK;
}

/* ===================== app_main ===================== */

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 Cisco OOB Console (USB-OTG) starting...");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    config_init();
    app_config_t cfg;
    config_load(&cfg);
    current_baud = cfg.baud_rate;

    if (cfg.otg_enabled) {
        usb_host_init_delayed();
        otg_enabled = true;
    } else {
        otg_enabled = false;
        ESP_LOGI(TAG, "USB OTG deaktiviert — in Einstellungen aktivieren + Neustart");
    }

    /* WiFi */
    if (config_consume_boot_ap())
        wifi_start_ap_direct();
    else
        wifi_init_sta();

    /* W5500 SPI-Ethernet (IP_EVENT_ETH_GOT_IP wird vom wifi_event_handler mitbehandelt) */
    if (eth_w5500_init(cfg.eth_mac[0] ? cfg.eth_mac : NULL) != ESP_OK) {
        ESP_LOGW(TAG, "W5500 nicht gefunden oder nicht angeschlossen — nur WiFi aktiv");
    }

    ESP_LOGI(TAG, "Ready. Connect USB-C to Cisco console port.");
    ESP_LOGI(TAG, "Web terminal: http://<wifi-ip>/  oder  http://<eth-ip>/");
}
