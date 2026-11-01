idf_component_register(SRCS "camera_probe.c"
                       INCLUDE_DIRS "."
                       REQUIRES espressif__esp32_p4_function_ev_board
                                esp_driver_cam
                                esp_driver_i2c
                                esp_driver_isp
                                esp_driver_ppa
                                esp_lcd
                                esp_hw_support
                                esp_psram
                                esp_timer
                                espressif__esp_cam_sensor
                                espressif__esp_ipa
                                espressif__esp_sccb_intf
                                heap
                                log)
