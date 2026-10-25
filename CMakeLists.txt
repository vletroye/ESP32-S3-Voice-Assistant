# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/bsp
    )
add_compile_options(-fdiagnostics-color=always -Wno-ignored-qualifiers)
project(chatgpt_demo)

# LVGL allocates its objects in PSRAM (main/lvgl_psram/lv_psram.h, selected by CONFIG_LV_MEM_CUSTOM_INCLUDE)
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/main/lvgl_psram")
target_compile_definitions(${lvgl_lib} PRIVATE
    LV_MEM_CUSTOM_ALLOC=lv_psram_malloc
    LV_MEM_CUSTOM_FREE=lv_psram_free
    LV_MEM_CUSTOM_REALLOC=lv_psram_realloc)

