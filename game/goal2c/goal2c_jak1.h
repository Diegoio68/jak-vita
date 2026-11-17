#pragma once

/*!
 * @file goal2c_jak1.h
 * goal2c (Jak Vita): modo mixto del runtime de PC para Jak 1.
 *
 * Si la variable de entorno GOAL2C_LIB apunta a una librería generada con
 * goalc/c_backend/tools/build_pc_lib.py, después de enlazar y ejecutar el top-level de cada
 * objeto se sustituyen sus funciones y métodos x86 (de goalc) por la versión en C de goal2c.
 * El resto del juego sigue en x86: las llamadas C -> x86 y x86 -> C pasan por trampolines.
 * Sin GOAL2C_LIB no hace nada.
 */

struct ObjectFileHeader;

namespace goal2c_runtime {

//! Llamar justo después de ejecutar el top-level de un objeto v3 de Jak 1.
void after_link_jak1(const char* object_name, ObjectFileHeader* ofh);

//! Funciones de ese objeto que se sustituyeron por C en su último enlace (para tests).
int replaced_functions(const char* object_name);

//! Huella del fotograma (validación x86 frente a C): si GOAL2C_HUELLA está definida, recorre la
//! cadena DMA que el juego envía al renderer y escribe en el log "[huella] <n> <hash>". Se
//! llama desde send_gfx_dma_chain (kmachine). La cadena contiene la geometría, las matrices de
//! cámara, los colores... de todo lo que se dibuja: si algo calcula distinto en C, cambia.
void frame_fingerprint(unsigned int chain);

//! Con GOAL2C_HUELLA: reloj virtual para __read-ee-timer (300 MHz). Avanza exactamente 1/60 s
//! por fotograma (en cada frame_fingerprint) y un poco en cada lectura, así que el tiempo del
//! juego no depende del tiempo real. Devuelve false si el modo huella no está activo.
bool virtual_ee_timer(unsigned long long* ticks);

}  // namespace goal2c_runtime
