/*!
 * @file goal2c_rt.h
 * Runtime mínimo que necesita el C generado por goal2c (Jak Vita).
 *
 * Modelo de ejecución (igual que goalc en x86):
 * - Los registros GPR de GOAL son de 64 bits. Una dirección GOAL es un offset de 32 bits
 *   respecto a la base de la memoria del juego (en x86, el "offset register" r15).
 *   Aquí esa base es g2c_mem: dirección real = g2c_mem + (u32)direccion_goal.
 * - Los floats viajan en GPRs como su patrón de bits (32 bits bajos), igual que en PS2.
 * - Los símbolos se resuelven al enlazar: g2c_rt_symbol("nombre") devuelve la dirección
 *   GOAL del símbolo (su valor vive en esa dirección, 32 bits).
 *
 * G2C_GPR32: compila los GPR como 32 bits en vez de 64. NO es semánticamente equivalente
 * (GOAL tiene aritmética de 64 bits); solo existe para medir en la Vita el coste de u64.
 *
 * Este fichero se compila como C11 (código generado) y también se puede incluir desde C++.
 */
#ifndef GOAL2C_RT_H
#define GOAL2C_RT_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef G2C_GPR32
typedef uint32_t g2c_gpr;
typedef int32_t g2c_sgpr;
#define G2C_SHIFT_MASK 31
#else
typedef uint64_t g2c_gpr;
typedef int64_t g2c_sgpr;
#define G2C_SHIFT_MASK 63
#endif

/* Base de la memoria GOAL. La define el runtime (o el arnés de pruebas). */
extern uint8_t* g2c_mem;

/* Dirección GOAL de un símbolo por nombre. La define el runtime (o el arnés). */
uint32_t g2c_rt_symbol(const char* name);

/* Dirección GOAL del objeto estático número idx (orden de FileEnv::statics) del fichero
   obj_name, apuntando a su comienzo. La define el runtime (o el arnés). */
uint32_t g2c_rt_static(const char* obj_name, int idx);

/* Puntero a la función C nativa de un objeto función de GOAL. Convención goal2c: la zona
   de código del objeto función (la dirección a la que apunta el valor GOAL) guarda el
   puntero nativo. El runtime de PC puede redefinirlo para el modo mixto x86 + C. */
#ifndef G2C_NATIVE_FN
#define G2C_NATIVE_FN(goal_addr) (*(void**)G2C_ADDR(goal_addr))
#endif

/* Entrada de la tabla de funciones de un fichero generado. */
typedef struct {
  const char* name;  /* nombre GOAL (p. ej. "lerp-clamp") */
  void* fn;          /* puntero a la función C */
  int nargs;         /* número de argumentos (todos g2c_gpr) */
} g2c_func_entry;

/* ---- conversiones de bits ---- */
static inline float g2c_bits_to_f(uint32_t u) {
  float f;
  memcpy(&f, &u, 4);
  return f;
}
static inline uint32_t g2c_f_to_bits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

/* ---- memoria GOAL (memcpy: seguro con accesos no alineados en ARM; gcc lo optimiza) ---- */
#define G2C_ADDR(a) (g2c_mem + (uint32_t)(a))
#define G2C_DEF_LOAD(name, T)                   \
  static inline T name(g2c_gpr a) {             \
    T v;                                        \
    memcpy(&v, G2C_ADDR(a), sizeof(T));         \
    return v;                                   \
  }
#define G2C_DEF_STORE(name, T)                  \
  static inline void name(g2c_gpr a, T v) {     \
    memcpy(G2C_ADDR(a), &v, sizeof(T));         \
  }
G2C_DEF_LOAD(g2c_ld_u8, uint8_t)
G2C_DEF_LOAD(g2c_ld_s8, int8_t)
G2C_DEF_LOAD(g2c_ld_u16, uint16_t)
G2C_DEF_LOAD(g2c_ld_s16, int16_t)
G2C_DEF_LOAD(g2c_ld_u32, uint32_t)
G2C_DEF_LOAD(g2c_ld_s32, int32_t)
G2C_DEF_LOAD(g2c_ld_u64, uint64_t)
G2C_DEF_LOAD(g2c_ld_f32, float)
G2C_DEF_STORE(g2c_st_u8, uint8_t)
G2C_DEF_STORE(g2c_st_u16, uint16_t)
G2C_DEF_STORE(g2c_st_u32, uint32_t)
G2C_DEF_STORE(g2c_st_u64, uint64_t)
G2C_DEF_STORE(g2c_st_f32, float)

/* ---- enteros: mismas reglas que goalc x86 ---- */
/* División de 32 bits. x86 lanzaría excepción con /0 o INT_MIN/-1 (comportamiento indefinido
   en C); aquí se devuelve un valor fijo para no tener UB. La PS2 no lanza excepción. */
static inline int32_t g2c_idiv32(int32_t a, int32_t b) {
  if (b == 0) return 0;
  if (a == INT32_MIN && b == -1) return INT32_MIN;
  return a / b;
}
static inline int32_t g2c_imod32(int32_t a, int32_t b) {
  if (b == 0) return 0;
  if (a == INT32_MIN && b == -1) return 0;
  return a % b;
}
static inline uint32_t g2c_udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint32_t g2c_umod32(uint32_t a, uint32_t b) { return b ? a % b : 0; }

/* float -> int32 truncando, con la semántica de cvttss2si (x86): NaN o fuera de rango
   dan INT32_MIN. En C el cast directo sería UB. */
static inline int32_t g2c_f2i(float f) {
  if (f >= -2147483648.0f && f < 2147483648.0f) return (int32_t)f;
  return INT32_MIN;
}

/* ---- comparaciones de float con la semántica de comiss + saltos sin signo (x86) ----
   Con NaN (no ordenados): EQ, LT y LE son ciertos; NE, GT y GE son falsos. */
#define G2C_FEQ(a, b) (!((a) < (b) || (a) > (b)))
#define G2C_FNE(a, b) ((a) < (b) || (a) > (b))
#define G2C_FLT(a, b) (!((a) >= (b)))
#define G2C_FLE(a, b) (!((a) > (b)))
#define G2C_FGT(a, b) ((a) > (b))
#define G2C_FGE(a, b) ((a) >= (b))

#ifdef __cplusplus
}
#endif

#endif /* GOAL2C_RT_H */
