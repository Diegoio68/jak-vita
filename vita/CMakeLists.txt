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
target_link_libraries(jakvita
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

vita_create_self(eboot.bin jakvita UNSAFE)
vita_create_vpk(jakvita.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
