idf_component_register(SRCS "main.c" "dht11.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash driver esp_timer common)
