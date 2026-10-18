# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

# Initialize the SDK
include(pico_sdk_import.cmake)

if (NOT PICO_TINYUSB_PATH)
    if (DEFINED ENV{PICO_TINYUSB_PATH})
        set(PICO_TINYUSB_PATH "$ENV{PICO_TINYUSB_PATH}")
    else()
        set(PICO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/tinyusb")
    endif()
endif()
set(PICO_TINYUSB_PATH "${PICO_TINYUSB_PATH}" CACHE PATH "Path to TinyUSB")

project(pico_lab)
pico_sdk_init()

# Let the SDK status LED source consume the same hardware macros as the application.
set_property(SOURCE
    ${PICO_SDK_PATH}/src/rp2_common/pico_status_led/status_led.c
    APPEND PROPERTY COMPILE_OPTIONS
    "-include" "${CMAKE_CURRENT_LIST_DIR}/board_config.h"
)

# Define our executable and its source files
add_executable(
    pico_lab
    main.c
    usb_descriptors.c
    dhserver.c
    dnserver.c
    ${PICO_TINYUSB_PATH}/lib/networking/rndis_reports.c
)

set(PICOLAB_VERSION "1.0.0")
target_compile_definitions(pico_lab PRIVATE
    PICOLAB_FIRMWARE_VERSION="${PICOLAB_VERSION}"
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
    PICO_USB_RESET_SUPPORT_MS_OS_20_DESCRIPTOR=0
    PICO_USB_RESET_INCLUDE_DEFAULT_APP_DRIVER_CB=0
)

pico_generate_pio_header(pico_lab ${CMAKE_CURRENT_LIST_DIR}/logic_analyzer.pio)

# Disable standard standard USB I/O to prevent conflicts
pico_enable_stdio_usb(pico_lab 0)
pico_enable_stdio_uart(pico_lab 0)

# Tell CMake where to find tusb_config.h and lwipopts.h
target_include_directories(pico_lab PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${PICO_TINYUSB_PATH}/lib/networking
)

# Create a dummy library for HTML content
pico_add_library(pico_httpd_content NOFLAG)

# Convert raw web files into C code automatically
pico_set_lwip_httpd_content(pico_httpd_content INTERFACE 
    ${CMAKE_CURRENT_LIST_DIR}/html/index.html
    ${CMAKE_CURRENT_LIST_DIR}/html/style.css
    ${CMAKE_CURRENT_LIST_DIR}/html/app.js
    ${CMAKE_CURRENT_LIST_DIR}/html/ok.json
    ${CMAKE_CURRENT_LIST_DIR}/html/winbox.js
    ${CMAKE_CURRENT_LIST_DIR}/html/winbox.min.css
    ${CMAKE_CURRENT_LIST_DIR}/html/wallpaper.svg
    ${CMAKE_CURRENT_LIST_DIR}/html/pinout_pico2.webp
    ${CMAKE_CURRENT_LIST_DIR}/html/pinout_picolab.webp
)

# Link the necessary Pico SDK, TinyUSB, and lwIP libraries
target_link_libraries(pico_lab PRIVATE
    pico_stdlib
    tinyusb_device
    tinyusb_board
    pico_lwip
    pico_lwip_http
    pico_httpd_content
    hardware_pwm
    hardware_adc
    hardware_dma
    hardware_uart
    hardware_pio
    pico_multicore
    pico_sync
    hardware_i2c
    hardware_spi
    pico_status_led
    pico_usb_reset
)

# Add extra build steps to generate UF2, BIN, etc.
pico_add_extra_outputs(pico_lab)