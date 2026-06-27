idf_component_register(SRCS "main.c" "config.c" "eth_w5500.c" "wg_client.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "web_terminal.html"
                    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_timer usb esp_eth driver trombik__esp_wireguard lwip)
