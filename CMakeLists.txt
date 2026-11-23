idf_component_register(
    SRCS 
        "src/audio_mixer.c"
    INCLUDE_DIRS 
        "include"
    REQUIRES
        freertos
        esp_common
        log
)

