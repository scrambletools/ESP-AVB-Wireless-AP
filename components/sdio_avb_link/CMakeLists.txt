idf_component_register(SRCS "sdio_avb_link.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_sdio sdmmc)
