# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# Load the project's own config profile on top of the upstream defaults.
# IDF only reads sdkconfig.defaults and sdkconfig.defaults.<target> by
# default, so sdkconfig.defaults.thai (board type, language, OTA URL, wake
# word, LVGL widgets the radar app needs) was easy to forget and silently
# produced a build for the wrong board. IDF appends "<entry>.<target>" for
# each entry itself, so sdkconfig.defaults.esp32s3 is still picked up and
# .thai stays last, which is what makes it win over sdkconfig.defaults.
# An explicit -DSDKCONFIG_DEFAULTS=... on the command line still overrides.
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.thai")
endif()

set(PROJECT_VER "2.4.0")
project(xiaozhi)
