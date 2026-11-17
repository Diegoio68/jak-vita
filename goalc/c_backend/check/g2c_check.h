/*!
 * @file g2c_check.h
 * goal2c (Jak Vita): se incluye (con -include) en cada fichero generado al compilarlo para la
 * app de comprobación (g2c_check.c). Redefine el acceso a memoria y la llamada a funciones:
 * - G2C_ADDR: dirección dentro de la imagen grabada; fuera de ella solo vale la pila de C
 *   (variables de pila de GOAL). Cualquier otra cosa marca el caso como inválido.
 * - G2C_NATIVE_FN: solo los objetos función parcheados con el C (marca G2C_CHECK_MAGIC); el
 *   resto (código x86 de la imagen, funciones del kernel) marca el caso como inválido.
 */
#ifndef G2C_CHECK_H
#define G2C_CHECK_H

#include <stdint.h>

#define G2C_CHECK_MAGIC 0x46433247u /* "G2CF" */

uint8_t* g2c_check_addr(uintptr_t a);
void* g2c_check_fn(uintptr_t a);

#define G2C_ADDR(a) g2c_check_addr((uintptr_t)(a))
#define G2C_NATIVE_FN(a) g2c_check_fn((uintptr_t)(a))

#endif
