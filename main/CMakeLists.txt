# Define source files
set(SOURCES "ble/ble_manager.cc"
            "ble/ancs_client.cc"
            "ble/ancs_defs.cc"
            "network/wifi_service.cc"
            "notification/notification.cc"
            "notification/webhook_client.cc"
            "peripherals/button.cc"
            "application.cc"
            "settings.cc"
            "main.cc"
            )

set(INCLUDE_DIRS "." "ble" "network" "notification" "peripherals")

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    REQUIRES bt nvs_flash esp_driver_gpio esp_psram esp_wifi esp_netif esp_event esp_timer esp_http_client esp-tls json app_update
                    )

# Pass the project version to the source code
target_compile_definitions(${COMPONENT_LIB} PRIVATE PROJECT_VERSION="${PROJECT_VER}")
