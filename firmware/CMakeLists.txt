cmake_minimum_required(VERSION 3.20)
project(MCU_BOOT_F412 C ASM)
set(CMAKE_C_STANDARD 11)
set(HAL Drivers/STM32F4xx_HAL_Driver)
set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
add_compile_options(${CPU_FLAGS} -ffunction-sections -fdata-sections -Wall -Wextra)
add_compile_definitions(USE_HAL_DRIVER STM32F412Rx)
include_directories(Core/Inc ${HAL}/Inc Drivers/CMSIS/Include
    Drivers/CMSIS/Device/ST/STM32F4xx/Include Drivers/mbedtls_sha256/include)
set(HAL_SOURCES)
foreach(module hal hal_can hal_cortex hal_dma hal_dma_ex hal_exti hal_flash
        hal_flash_ex hal_flash_ramfunc hal_gpio hal_pwr hal_pwr_ex hal_rcc hal_rcc_ex hal_uart)
    list(APPEND HAL_SOURCES ${HAL}/Src/stm32f4xx_${module}.c)
endforeach()

set(PLATFORM_SOURCES ${HAL_SOURCES} Core/Src/system_stm32f4xx.c
    Core/Src/syscalls.c Core/Src/sysmem.c Core/Startup/startup_stm32f412retx.s)
set(SHA_SOURCES Drivers/mbedtls_sha256/library/sha256.c Drivers/mbedtls_sha256/library/platform_util.c)
add_executable(bootloader ${PLATFORM_SOURCES} ${SHA_SOURCES}
    Core/Src/main.c Core/Src/stm32f4xx_it.c Core/Src/stm32f4xx_hal_msp.c
    Core/Src/boot_flash.c Core/Src/boot_port.c Core/Src/boot_update.c Core/Src/bootloader.c)
set(script ${CMAKE_SOURCE_DIR}/STM32F412RETX_FLASH.ld)
set_target_properties(bootloader PROPERTIES SUFFIX .elf LINK_DEPENDS "${script}")
target_link_options(bootloader PRIVATE ${CPU_FLAGS} -T${script} --specs=nano.specs --specs=nosys.specs
    -Wl,--gc-sections -Wl,-Map=bootloader.map -Wl,--print-memory-usage)
add_custom_command(TARGET bootloader POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:bootloader> bootloader.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:bootloader> bootloader.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:bootloader>)

# QEMU tests (mps2-an386): upload/install logic, and heartbeat/diagnostic payloads.
add_executable(boot_tests EXCLUDE_FROM_ALL ${SHA_SOURCES}
    Core/Src/boot_update.c Core/Src/bootloader.c tests/test_boot.c tests/startup.c)
add_executable(boot_port_tests EXCLUDE_FROM_ALL ${SHA_SOURCES}
    Core/Src/boot_update.c Core/Src/bootloader.c tests/test_heartbeat.c tests/startup.c)
foreach(test_target boot_tests boot_port_tests)
    target_compile_options(${test_target} PRIVATE -UNDEBUG)
    target_link_options(${test_target} PRIVATE ${CPU_FLAGS} -nostartfiles --specs=rdimon.specs
        -T${CMAKE_SOURCE_DIR}/tests/qemu.ld -Wl,--gc-sections)
    set_target_properties(${test_target} PROPERTIES SUFFIX .elf LINK_DEPENDS "${CMAKE_SOURCE_DIR}/tests/qemu.ld")
endforeach()
