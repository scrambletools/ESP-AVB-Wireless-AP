set(srcs "avb_bridge_main.c")

if(CONFIG_AVB_BRIDGE_PERF_TEST_MODE)
    list(APPEND srcs "avb_bridge_perf_test.c")
endif()

idf_component_register(SRCS ${srcs})
