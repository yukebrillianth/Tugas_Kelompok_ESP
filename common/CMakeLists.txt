idf_component_register(SRCS "mesh_bt.c" "rgb_led.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bt esp_driver_rmt)
