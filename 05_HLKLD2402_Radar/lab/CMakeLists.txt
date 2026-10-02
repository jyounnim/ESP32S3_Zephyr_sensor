# PRO_CPU (core 0) image = sensor core. Sysbuild adds the APP_CPU image (remote/).
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ld2402_sensor_procpu)

target_include_directories(app PRIVATE common src)
target_sources(app PRIVATE
	src/sensor_main.c
	src/ld2402.c
	src/radar_trend.c
	src/radar_filter.c
	common/radar_link_ipm.c
)
