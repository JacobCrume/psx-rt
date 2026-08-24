cmake_minimum_required(VERSION 3.21)
project(cputest LANGUAGES C)
file(GLOB _sources *.c)
psn00bsdk_add_executable(cputest GPREL ${_sources})
