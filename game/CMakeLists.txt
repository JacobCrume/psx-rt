cmake_minimum_required(VERSION 3.21)

project(
	game
	LANGUAGES    C
	VERSION      1.0.0
	DESCRIPTION  "Realtime ray tracing demo on real PS1 hardware constraints"
)

file(GLOB _sources *.c)
psn00bsdk_add_executable(game GPREL ${_sources})

install(FILES ${PROJECT_BINARY_DIR}/game.exe TYPE BIN)
