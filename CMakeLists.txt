cmake_minimum_required(VERSION 3.22)

# 复用统一错误码：已有 target → 同级源码 → 固定版本拉取。
if(NOT TARGET stm_common)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../stm_common/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../stm_common"
                         "${CMAKE_CURRENT_BINARY_DIR}/stm_common")
    else()
        option(STM_COMMON_FETCH "缺少本地依赖时允许拉取 stm_common" ON)
        set(STM_COMMON_GIT_REPOSITORY "https://github.com/NingZiXi/stm_common.git"
            CACHE STRING "stm_common 仓库地址")
        if(NOT STM_COMMON_FETCH)
            message(FATAL_ERROR "stm_common 缺失：请提供 target、同级源码或开启 STM_COMMON_FETCH")
        endif()
        include(FetchContent)
        FetchContent_Declare(stm_common
            GIT_REPOSITORY "${STM_COMMON_GIT_REPOSITORY}"
            GIT_TAG ce3d186dde2d374a8e9c7b9068a7b88f97d57dc1 # v1.0.0
        )
        FetchContent_MakeAvailable(stm_common)
    endif()
endif()
if(NOT TARGET stm_common)
    message(FATAL_ERROR "依赖未提供 stm_common target")
endif()


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

# 日志统一使用 stm_log，STM_LOG_ENABLED 由应用通过 PUBLIC 编译宏传递。

# C11
target_compile_features(stm_ota PUBLIC c_std_11)

target_link_libraries(stm_ota PUBLIC stm_common)

# 统一日志依赖：先复用 target，再查找同级源码；不隐式选择芯片或输出后端。
if(NOT TARGET stm_log)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../stm_log/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../stm_log"
                         "${CMAKE_CURRENT_BINARY_DIR}/stm_log")
    else()
        message(FATAL_ERROR "请先提供 stm_log target 或同级 stm_log 源码")
    endif()
endif()
target_link_libraries(stm_ota PUBLIC stm_log)
