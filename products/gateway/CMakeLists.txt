# Gateway product (ESP32-S3 + ILI9488 TFT, ESP-NOW receiver).
# A standalone ESP-IDF project: it owns its app_main() (main/gateway_main.c) and
# pulls the shared code in as components via main/idf_component.yml `path:`
# dependencies (../../common_components). Build artifact: build/gateway.bin
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gateway)
