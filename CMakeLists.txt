#
# Copyright (c) 2025 Microchip Technology Inc.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ble_stepper_motor_mppb)

target_sources(app PRIVATE
  src/main.c
  src/app_stepper_motor.c
  src/trs.c
  src/trs_server.c
  )

zephyr_library_include_directories(.)