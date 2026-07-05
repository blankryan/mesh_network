# Worker product (ESP32 + SSD1306 OLED, ESP-NOW sensor node).
# A standalone ESP-IDF project: it owns its app_main() (main/worker_main.c) and
# pulls the shared code in as components via main/idf_component.yml `path:`
# dependencies (../../common_components). Build artifact: build/worker.bin
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(worker)
