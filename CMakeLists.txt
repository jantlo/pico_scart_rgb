# cmake version
cmake_minimum_required(VERSION 3.13)

# include the sdk.cmake file
include(pico_sdk_import.cmake)

# give the project a name (anything you want)
project(scart-pio-project)

# initialize the sdk
pico_sdk_init()

#######

# name anything you want
add_executable(scart_rgb
	pico_sdk_import.cmake csync.pio
	rgb.pio)

# must match with pio filename and executable name from above
pico_generate_pio_header(scart_rgb ${CMAKE_CURRENT_LIST_DIR}/csync.pio)
pico_generate_pio_header(scart_rgb ${CMAKE_CURRENT_LIST_DIR}/rgb.pio)

# must match with executable name and source file names
target_sources(scart_rgb PRIVATE scart_rgb.c)

# must match with executable name
target_link_libraries(scart_rgb PRIVATE pico_stdlib hardware_pio hardware_dma)

# must match with executable name
pico_add_extra_outputs(scart_rgb)
