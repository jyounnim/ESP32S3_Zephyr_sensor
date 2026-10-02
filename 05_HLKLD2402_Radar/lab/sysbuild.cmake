# Build the APP_CPU (display core) image from remote/ alongside this image.
# Same form as the hardware-verified multi-sensor AMP labs: the remote board
# target is spelled out explicitly instead of being derived from
# BOARD_QUALIFIERS (string manipulation there is fragile across Zephyr
# versions and produced an invalid board name).

ExternalZephyrProject_Add(
	APPLICATION remote
	SOURCE_DIR  ${APP_DIR}/remote
	BOARD       esp32s3_devkitc/esp32s3/appcpu
)
