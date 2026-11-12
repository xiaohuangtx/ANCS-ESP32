# Define source files
set(SOURCES "ble/ble_manager.cc"
            "ble/ancs_client.cc"
            "ble/ancs_defs.cc"
            "network/wifi_service.cc"
            "network/wifi_store.cc"
            "network/config_portal.cc"
            "network/dns_server.cc"
            "notification/notification.cc"
            "notification/notification_history.cc"
            "notification/notification_store.cc"
            "notification/webhook_client.cc"
            "peripherals/button.cc"
            "web/web_server.cc"
            "web/http_utils.cc"
            "application.cc"
            "settings.cc"
            "log_buffer.cc"
            "main.cc"
            )

set(INCLUDE_DIRS "." "ble" "network" "notification" "peripherals" "web")

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    EMBED_TXTFILES "web/admin.html" "network/config_portal.html"
                    REQUIRES bt nvs_flash esp_driver_gpio esp_psram esp_wifi esp_netif lwip esp_event esp_timer esp_http_client esp_http_server esp-tls cjson app_update esp_partition
                    )

# Pass the project version to the source code
target_compile_definitions(${COMPONENT_LIB} PRIVATE PROJECT_VERSION="${PROJECT_VER}")
