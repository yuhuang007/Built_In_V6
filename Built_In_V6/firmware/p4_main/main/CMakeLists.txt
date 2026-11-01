set(app_sources "main.c" "font_project_zh.c" "font_startup_zh.c" "p4_font_loader.c"
                "chat_history.c" "chat_store_codec.c" "device_about.c" "screen_idle.c" "screen_idle_policy.c"
                "wifi_settings.c" "text_entry.c" "usb_keyboard.c" "cloud_text.c"
                "pinyin_service.c" "pinyin_asset.c")
set(sign_dependencies esp_driver_cam esp_driver_isp esp_driver_ppa
    esp_driver_i2c esp_timer espressif__esp_cam_sensor espressif__esp_ipa
    espressif__esp_sccb_intf)
if(CONFIG_P4_SIGN_LANGUAGE_MODE)
    list(APPEND app_sources "sign_language_mode.c" "portrait_display.c")
endif()
if(CONFIG_P4_ASR_MODE)
    list(APPEND app_sources "doubao_asr.c" "speech_to_text_mode.c" "transcript_turns.c"
                            "asr_display_font.c" "font_asr_symbols.c" "sd_font_upgrade.c")
endif()

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                       REQUIRES espressif__esp32_p4_function_ev_board
                                esp_event
                                esp_http_client
                                tcp_transport
                                esp-tls
                                espressif__zlib
                                esp_netif
                                json
                                esp_hw_support
                                esp_psram
                                esp_system
                                app_update
                                esp_app_format
                                espressif__esp_codec_dev
                                heap
                                log
                                lvgl__lvgl
                                mbedtls
                                nvs_flash
                                p4_pinyin
                                esp_wifi
                                spi_flash
                                volc_conv_ai
                                ${sign_dependencies})

# Glyph-cache lookups and bitmap row copies run on every scrolling frame.
if(CONFIG_P4_ASR_MODE)
    set_source_files_properties(asr_display_font.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()

execute_process(COMMAND git rev-parse --short=7 HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE p4_build_git OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT p4_build_git MATCHES "^[0-9a-f]+$")
    set(p4_build_git "unknown")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE P4_BUILD_GIT="${p4_build_git}")

if(CONFIG_P4_SIGN_LANGUAGE_MODE)
    # Keep per-pixel portrait transport optimized even in the debug profile.
    set_source_files_properties(portrait_display.c PROPERTIES COMPILE_OPTIONS "-O2")
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=lvgl_port_add_disp_dsi"
        "-Wl,--undefined=__wrap_lvgl_port_add_disp_dsi"
        "-Wl,--wrap=dw_gdma_channel_register_event_callbacks"
        "-Wl,--undefined=__wrap_dw_gdma_channel_register_event_callbacks")
endif()

if(CONFIG_LV_USE_GIF)
    target_add_binary_data(${COMPONENT_LIB}
        "${CMAKE_CURRENT_LIST_DIR}/../../../assets/startup/loading.gif" BINARY
        RENAME_TO startup_loading_gif)
endif()
