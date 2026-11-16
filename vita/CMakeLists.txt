# Jak Vita — hello world de Fase 0.
# Proyecto independiente del CMake de upstream: se configura aparte con la
# toolchain de VitaSDK para no tocar el build de PC.
#
#   cmake -S vita -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#   cmake --build build-vita
#
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    set(CMAKE_TOOLCHAIN_FILE "/usr/local/vitasdk/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  endif()
endif()

project(jakvita C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Jak Vita")
set(VITA_TITLEID "JAKV00001")
set(VITA_VERSION "00.01")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O2 -fno-exceptions")

add_executable(jakvita
  src/main.cpp
  src/log.cpp
)

# Orden importante: vitaGL depende de vitashark/mathneon y de los stubs del sistema.
set(JAKVITA_GL_LIBS
  vitaGL
  vitashark
  SceShaccCgExt
  mathneon
  taihen_stub
  SceShaccCg_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceDisplay_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceSysmodule_stub
  SceIofilemgr_stub
  SceCtrl_stub
  ScePower_stub
  m
  z
)
target_link_libraries(jakvita ${JAKVITA_GL_LIBS})

vita_create_self(eboot.bin jakvita UNSAFE)
vita_create_vpk(jakvita.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)

# ---------------------------------------------------------------------------
# Benchmark goal2c (Fase 1): JAKV00002, log en ux0:data/jakvita/bench.txt
# Necesita el C generado y la referencia de goalc, que crea el test de PC:
#   build/goalc-test --gtest_filter='Goal2CTests.*'
# ---------------------------------------------------------------------------
set(G2C_OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../out/goal2c/jak1" CACHE PATH "salida de goal2c")
set(G2C_MATH_C "${G2C_OUT_DIR}/math.c")
if(EXISTS "${G2C_MATH_C}" AND EXISTS "${G2C_OUT_DIR}/math_expected.h")
  set(G2C_RT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../goalc/c_backend/runtime")
  set(G2C_C_FLAGS -std=c11 -O2 -Wall -Wextra -Werror)

  # el mismo fichero generado, compilado dos veces
  add_library(g2c_math64 OBJECT "${G2C_MATH_C}")
  target_include_directories(g2c_math64 PRIVATE "${G2C_RT_DIR}")
  target_compile_options(g2c_math64 PRIVATE ${G2C_C_FLAGS})

  add_library(g2c_math32 OBJECT "${G2C_MATH_C}")
  target_include_directories(g2c_math32 PRIVATE "${G2C_RT_DIR}")
  target_compile_options(g2c_math32 PRIVATE ${G2C_C_FLAGS})
  target_compile_definitions(g2c_math32 PRIVATE
    G2C_GPR32 g2c_math_funcs=g2c_math_funcs32 g2c_math_link=g2c_math_link32
    g2c_rt_symbol=g2c_rt_symbol32 g2c_rt_static=g2c_rt_static32 g2c_math_info=g2c_math_info32)

  add_executable(jakvita_bench
    bench/main.cpp
    src/log.cpp
    $<TARGET_OBJECTS:g2c_math64>
    $<TARGET_OBJECTS:g2c_math32>
  )
  target_include_directories(jakvita_bench PRIVATE "${G2C_OUT_DIR}" "${G2C_RT_DIR}")
  target_link_libraries(jakvita_bench ${JAKVITA_GL_LIBS})

  vita_create_self(bench_eboot.bin jakvita_bench UNSAFE)
  vita_create_vpk(jakvita_bench.vpk JAKV00002 bench_eboot.bin
    VERSION ${VITA_VERSION}
    NAME "Jak Vita bench"
  )
else()
  message(STATUS "goal2c: no está ${G2C_MATH_C}; no se compila el benchmark "
                 "(ejecuta antes build/goalc-test --gtest_filter='Goal2CTests.*')")
endif()
