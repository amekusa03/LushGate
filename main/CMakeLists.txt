# ===========================================================
# LushGate - BLE専用ビルド (超低消費電力・電池駆動最適化)
# ===========================================================
idf_component_register(
    SRCS
        "main.c"
        "rain_sensor.c"
        "pump_control.c"
        "storage_manager.c"
        "ble_gatt.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_adc
        driver
        esp_timer
        bt
)
