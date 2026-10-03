# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sam0_tcc_pwm_testbench)

target_sources(app PRIVATE src/main.c src/tcc_util.c src/test_driver.c src/report.c)
target_sources_ifdef(CONFIG_TB_UF2_REBOOT app PRIVATE src/uf2_reboot.c)

# A testbench driver variant replaces the in-tree driver.
foreach(variant main lupd stop direct hybrid)
  string(TOUPPER ${variant} VARIANT)
  if(CONFIG_TB_DRIVER_${VARIANT})
    if(CONFIG_PWM_SAM0_TCC)
      message(FATAL_ERROR "Driver variant '${variant}' needs CONFIG_PWM_SAM0_TCC=n. "
                          "Build with -DEXTRA_CONF_FILE=variants/${variant}.conf.")
    endif()
    target_sources(app PRIVATE variants/pwm_sam0_tcc_${variant}.c)
  endif()
endforeach()

if(CONFIG_TB_DRIVER_IN_TREE AND NOT CONFIG_PWM_SAM0_TCC)
  message(FATAL_ERROR "The in-tree driver is selected, but CONFIG_PWM_SAM0_TCC is not set.")
endif()
