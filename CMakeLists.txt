# hermes-core as a Zephyr module (module root is this repository root)
zephyr_library_named(hermes_core)
zephyr_library_sources(
    components/hermes_core/src/json.c
    components/hermes_core/src/agent.c
    components/hermes_core/src/skills.c
    components/hermes_core/src/history.c
)
zephyr_library_sources_ifdef(CONFIG_HL_PLATFORM_ZEPHYR
    components/hermes_core/src/platform_zephyr.c
)
zephyr_include_directories(
    components/hermes_core/include
    components/hermes_core/src
)
if(CONFIG_FILE_SYSTEM_LITTLEFS AND ZEPHYR_LITTLEFS_MODULE_DIR)
    zephyr_include_directories(${ZEPHYR_LITTLEFS_MODULE_DIR})
endif()
