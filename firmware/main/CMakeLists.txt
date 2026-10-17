idf_component_register(
    SRCS "main.c" "power.c" "buttons.c" "encoder.c" "led_status.c" "proto.c"
         "recorder.c" "player.c" "songbook.c" "wifi_share.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES json esp_driver_gpio esp_driver_pcnt esp_driver_rmt esp_timer nvs_flash
                  esp_partition esp_driver_i2s esp_driver_usb_serial_jtag
                  esp_wifi esp_http_server esp_netif esp_event
                  chmorgan__esp-libhelix-mp3
)
