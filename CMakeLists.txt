cmake_minimum_required(VERSION 4.0)

project ( KernelMPC5674F C CXX ASM )

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIBRARIES_DIR	    ${CMAKE_SOURCE_DIR}/libs)

include(FetchContent)
include(cmake/Runtime.cmake)

FetchContent_Declare(
    EmbeddedIOServices
    GIT_REPOSITORY https://github.com/FL0WL0W/EmbeddedIOServices
    GIT_TAG        main
    GIT_PROGRESS   TRUE
    GIT_SHALLOW    TRUE
    SOURCE_DIR  ${LIBRARIES_DIR}/EmbeddedIOServices
)

FetchContent_MakeAvailable(EmbeddedIOServices)

target_link_libraries(EmbeddedIOServices PUBLIC e200_runtime)
set(EmbeddedIOServicesHardwareAbstraction_Path "${embeddedioservices_SOURCE_DIR}/HardwareAbstractions/MPC5xxx")

include_directories(include)
include_directories(${EmbeddedIOServicesHardwareAbstraction_Path}/include)

file(GLOB_RECURSE USER_CSOURCES src/*.c)
file(GLOB_RECURSE USER_CSOURCES_Templates src/*_template.c)
IF(USER_CSOURCES_Templates)
        list(REMOVE_ITEM USER_CSOURCES ${USER_CSOURCES_Templates})
ENDIF()
file(GLOB_RECURSE USER_CXXSOURCES src/*.cpp)
file(GLOB_RECURSE USER_CXXSOURCES_Templates src/*_template.cpp)
IF(USER_CXXSOURCES_Templates)
        list(REMOVE_ITEM USER_CXXSOURCES ${USER_CXXSOURCES_Templates})
ENDIF()
set(EmbeddedIOServiceHardwareAbstraction_SRCS
    ${EmbeddedIOServicesHardwareAbstraction_Path}/src/MPC5xxxSystemClockService.cpp
    ${EmbeddedIOServicesHardwareAbstraction_Path}/src/MPC56xxSystemClockService.cpp
    ${EmbeddedIOServicesHardwareAbstraction_Path}/src/MPC5xxxFlexCAN2Service.cpp
    ${EmbeddedIOServicesHardwareAbstraction_Path}/src/MPC5xxxSPIService.cpp
)

set(SRC_FILES ${USER_CSOURCES} ${USER_CXXSOURCES} ${EmbeddedIOServiceHardwareAbstraction_SRCS} startup_mpc5674f.S MPC5674_RAM.ld)
add_executable(firmware.elf ${SRC_FILES})

set_property(TARGET firmware.elf PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/MPC5674_RAM.ld")

target_compile_definitions(firmware.elf PRIVATE MPC5674F)
target_link_libraries(firmware.elf EmbeddedIOServices)

set(HEX_FILE "${CMAKE_CURRENT_BINARY_DIR}/Kernel-E92.hex")
set(BIN_FILE "${CMAKE_CURRENT_BINARY_DIR}/Kernel-E92.bin")
add_custom_command(TARGET firmware.elf POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O ihex $<TARGET_FILE:firmware.elf> "${HEX_FILE}"
    COMMAND "${CMAKE_OBJCOPY}" -O binary $<TARGET_FILE:firmware.elf> "${BIN_FILE}"
    COMMAND "${CMAKE_SIZE}" $<TARGET_FILE:firmware.elf>
    BYPRODUCTS "${HEX_FILE}" "${BIN_FILE}"
    VERBATIM
)
