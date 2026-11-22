idf_component_register(SRCS "main.c" "mesh_prov.c" "gateway.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash driver esp_wifi esp_netif esp_event esp_timer mqtt json common)
