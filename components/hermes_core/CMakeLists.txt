idf_component_register(
    SRCS "src/json.c" "src/agent.c" "src/skills.c" "src/history.c"
         "src/platform_esp32.c"
    INCLUDE_DIRS "include" "src"
    PRIV_REQUIRES esp_http_client esp_timer esp-tls mbedtls
)
