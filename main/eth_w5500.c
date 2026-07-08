/*
 * W5500 SPI-Ethernet — Initialisierung für den ESP32-S3
 *
 * Verwendet die espressif/w5500-Komponente (Component Registry).
 * Die neue API übergibt SPI-Host + Device-Config direkt an den MAC-Treiber;
 * spi_bus_add_device() wird intern vom W5500-Treiber aufgerufen.
 */

#include "eth_w5500.h"
#include "config.h"

#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_mac_w5500.h"  /* espressif/w5500 – MAC + ETH_W5500_DEFAULT_CONFIG */
#include "esp_eth_phy_w5500.h"  /* espressif/w5500 – PHY                            */
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"

static const char *TAG = "eth_w5500";

/* Logging der Ethernet-Link-Ereignisse */
static void eth_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)data;
    uint8_t mac[6];

    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "Link Up — MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Link Down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Treiber gestartet");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Treiber gestoppt");
        break;
    default:
        break;
    }
}

esp_err_t eth_w5500_init(const char *custom_mac_str)
{
    esp_err_t ret;

    /* 1 — SPI2-Bus initialisieren (MOSI/MISO/SCLK) */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = W5500_MOSI_GPIO,
        .miso_io_num   = W5500_MISO_GPIO,
        .sclk_io_num   = W5500_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ret = spi_bus_initialize(W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI-Bus-Init fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2 — SPI-Device-Konfiguration
     *
     * Der W5500 nutzt ein proprietäres Frame-Format:
     *   16 Bit Adresse | 8 Bit Steuerbyte | n Byte Daten
     * => command_bits=16, address_bits=8.
     *
     * static: Pointer wird intern vom W5500-MAC-Treiber gespeichert.
     */
    static spi_device_interface_config_t devcfg = {
        .command_bits   = 16,
        .address_bits   = 8,
        .mode           = 0,
        .clock_speed_hz = W5500_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num   = W5500_CS_GPIO,
        .queue_size     = 20,
    };

    /* 3 — W5500 MAC (neue API: Host-ID + Device-Config-Pointer) */
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &devcfg);
    w5500_cfg.base.int_gpio_num = W5500_INT_GPIO;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "W5500-MAC konnte nicht erstellt werden");
        return ESP_FAIL;
    }

    /* 4 — W5500 PHY */
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = W5500_RST_GPIO;  /* -1 = RST nicht angeschlossen */
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "W5500-PHY konnte nicht erstellt werden");
        return ESP_FAIL;
    }

    /* 5 — Ethernet-Treiber installieren */
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ret = esp_eth_driver_install(&eth_cfg, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet-Treiber-Install fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 5b — MAC setzen: custom_mac_str (aus Einstellungen) oder eFuse-Fallback */
    uint8_t mac_addr[6];
    bool mac_from_config = false;

    if (custom_mac_str && custom_mac_str[0] != '\0') {
        /* Nutzer-definierte MAC parsen */
        unsigned int m[6] = {0};
        if (sscanf(custom_mac_str, "%x:%x:%x:%x:%x:%x",
                   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            for (int i = 0; i < 6; i++) mac_addr[i] = (uint8_t)m[i];
            mac_from_config = true;
        } else {
            ESP_LOGW(TAG, "Ungültige MAC '%s' — eFuse wird verwendet", custom_mac_str);
        }
    }

    if (!mac_from_config) {
        if (esp_read_mac(mac_addr, ESP_MAC_ETH) != ESP_OK) {
            ESP_LOGW(TAG, "esp_read_mac fehlgeschlagen — W5500-Default-MAC bleibt");
            mac_addr[0] = 0; /* Kenntlich machen dass kein Setzen stattfindet */
        }
    }

    if (mac_addr[0] != 0 || mac_addr[1] != 0) { /* mindestens nicht alle 0 */
        ret = esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "MAC gesetzt (%s): %02x:%02x:%02x:%02x:%02x:%02x",
                     mac_from_config ? "Einstellungen" : "eFuse",
                     mac_addr[0], mac_addr[1], mac_addr[2],
                     mac_addr[3], mac_addr[4], mac_addr[5]);
        } else {
            ESP_LOGW(TAG, "MAC konnte nicht gesetzt werden: %s", esp_err_to_name(ret));
        }
    }

    /* 6 — esp_netif-Instanz für Ethernet anlegen (DHCP-Client aktiv) */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    if (!eth_netif) {
        ESP_LOGE(TAG, "ETH-Netif konnte nicht erstellt werden");
        return ESP_FAIL;
    }

    esp_err_t mtu_err = esp_netif_set_mtu(eth_netif, NETIF_MTU);
    if (mtu_err == ESP_OK)
        ESP_LOGI(TAG, "MTU %d: ETH_DEF", NETIF_MTU);
    else
        ESP_LOGW(TAG, "MTU ETH_DEF: %s", esp_err_to_name(mtu_err));

    /* 7 — Glue-Layer: Ethernet-Treiber ↔ Netif verknüpfen */
    esp_eth_netif_glue_handle_t eth_glue = esp_eth_new_netif_glue(eth_handle);
    ret = esp_netif_attach(eth_netif, eth_glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Netif-Attach fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 8 — ETH-Events für Logging registrieren */
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);

    /* 9 — Ethernet starten */
    ret = esp_eth_start(eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet-Start fehlgeschlagen: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "W5500 gestartet — SPI%d, %d MHz, CS=GPIO%d INT=GPIO%d RST=GPIO%d",
             (W5500_SPI_HOST == SPI2_HOST) ? 2 : 3,
             W5500_SPI_CLOCK_MHZ,
             W5500_CS_GPIO, W5500_INT_GPIO, W5500_RST_GPIO);
    return ESP_OK;
}
