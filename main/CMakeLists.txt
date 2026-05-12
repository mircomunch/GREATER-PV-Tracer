idf_component_register(
    SRCS "continuous_read_main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_gptimer
    PRIV_REQUIRES esp_adc
)