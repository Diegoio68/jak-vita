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

//! Recorrido guionizado (GOAL2C_RECORRIDO=<guion>, con GOAL2C_HUELLA): sustituye la lectura del
//! mando 0 (cpad_info: CPadInfo en la memoria GOAL) por la del guion para este fotograma. Los
//! fotogramas del guion cuentan desde que existe *target* (Jak), así x86 y C reciben la misma
//! entrada en el mismo fotograma de juego. Formato en test/goal2c/recorrido.txt.
void scripted_pad(unsigned long long cpad_info);

//! Modo determinista (GOAL2C_HUELLA o GOAL2C_GRABAR): reloj virtual de 1/60 s por fotograma y
//! cargas del IOP que acaban en el fotograma en que se consultan (RpcBusy espera).
//! Con GOAL2C_GRABAR=<fichero>, scripted_pad graba el mando 0 en formato de recorrido.
bool deterministic_mode();

//! Sonido determinista (det_sound_jak1.cpp): en modo determinista *sound-iop-info* (strpos...)
//! lo calcula el EE a partir de las órdenes de stream del juego (RPC de canal 5), no el IOP.
bool det_sound_on();
void det_play_rpc(const void* send, int size);
void det_sound_frame();
void det_sound_reset();

}  // namespace goal2c_runtime
