# Build de HOST: roda o core do sensor no PC, sem hardware.
#
# O firmware do G4 NAO se compila por aqui -- ver firmware/platform/stm32/
# e docs/ARQUITETURA.md ("Como compilar para o G4").
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build

cmake_minimum_required(VERSION 3.16)
project(sensor_ir_goertzel_lockin C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wstrict-prototypes -Wmissing-prototypes
    -Wpointer-arith -Wcast-qual
    -O2 -g
)

# O core: zero dependencia de plataforma. Se algum dia isto precisar de um
# include de HAL para compilar, a arquitetura foi violada.
add_library(sensor_core STATIC
    firmware/core/dsp/fx_math.c
    firmware/core/dsp/goertzel.c
    firmware/core/dsp/lockin.c
    firmware/core/sensor/hysteresis.c
    firmware/core/sensor/threat_scan.c
    firmware/core/sensor/ir_sensor.c
)
target_include_directories(sensor_core PUBLIC
    firmware/core
    firmware/port
)

# Camada de aplicacao: independente de plataforma, entao compila no host
# tambem. Compilar aqui nao eh enfeite -- eh o que garante que app.c nao
# adquiriu dependencia de HAL sem ninguem notar.
add_library(sensor_app STATIC firmware/app/app.c)
target_link_libraries(sensor_app PUBLIC sensor_core)

# Simulador de front-end (so no host).
add_library(sim_host STATIC
    firmware/platform/host/sim_frontend.c
)
target_link_libraries(sim_host PUBLIC sensor_core)
target_link_libraries(sim_host PUBLIC m)

enable_testing()

foreach(t test_dsp test_sensor)
    add_executable(${t} firmware/test/${t}.c)
    target_link_libraries(${t} PRIVATE sensor_core sim_host m)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# Ferramenta de bancada: varre a cena e imprima CSV para plotar.
add_executable(sweep firmware/app/main_host.c)
target_link_libraries(sweep PRIVATE sensor_core sensor_app sim_host m)
