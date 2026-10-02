# APP_CPU (core 1) image = display core.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ld2402_display_appcpu)

target_include_directories(app PRIVATE ../common src)
target_sources(app PRIVATE
	src/display_main.c
	src/radar_display_ssd1306.c
	src/ssd1306_display.c
	../common/radar_link_ipm.c
)
