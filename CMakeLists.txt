cmake_minimum_required(VERSION 3.22)

# STM32 OTA 库（独立 git 仓库，被主项目 add_subdirectory 引入）

set(_STM_OTA_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(stm_ota STATIC
    ${_STM_OTA_DIR}/src/stm_ota.c
)

target_include_directories(stm_ota PUBLIC
    ${_STM_OTA_DIR}/inc
)

# 依赖 esp_at_client（OTA 调 HTTP GET 拉固件）
if(TARGET esp_at_client)
    target_link_libraries(stm_ota PUBLIC esp_at_client)
endif()

# STM32F4 HAL（stm_ota 内部用，限定 STM32F4 系列）
if(TARGET stm32cubemx)
    target_link_libraries(stm_ota PUBLIC stm32cubemx)
endif()

# 可选 stm_log（OTA 内部打日志）
if(TARGET stm_log)
    target_link_libraries(stm_ota PUBLIC stm_log)
endif()

# C11
target_compile_features(stm_ota PUBLIC c_std_11)
