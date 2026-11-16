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

/* pp: el proceso actual (registro r13 en goalc x86). El C generado lo lee y escribe como
   G2C_PP. El runtime fija g2c_pp_ptr: en el modo mixto de PC apunta a la variable del runtime
   que guarda r13 en cada entrada x86 -> C y lo pasa en cada llamada C -> x86. */
extern uint64_t* g2c_pp_ptr;
#define G2C_PP (*g2c_pp_ptr)

/* rsp y r15 de goalc x86, de solo lectura: el marco de C actual (en la misma pila que el
   proceso) y la base de la memoria GOAL. */
#define G2C_SP_REG ((g2c_gpr)(uintptr_t)__builtin_frame_address(0))
#define G2C_BASE_REG ((g2c_gpr)(uintptr_t)g2c_mem)

/* Dirección GOAL de un símbolo por nombre. La define el runtime (o el arnés). */
uint32_t g2c_rt_symbol(const char* name);

/* Dirección GOAL de un objeto estático del fichero obj_name: está en el segmento seg del
   objeto cargado, en la posición offset (la que le dio goalc al generar el objeto). La define
   el runtime (o el arnés). Devuelve el comienzo del objeto estático. */
uint32_t g2c_rt_static(const char* obj_name, int seg, int offset);

/* Entrada de la tabla de funciones de un fichero generado. */
typedef struct {
  const char* name;  /* nombre GOAL ("lerp-clamp") o "(method <id> <tipo>)" */
  void* fn;          /* puntero a la función C: g2c_gpr f(g2c_gpr a0, ...) */
  int nargs;         /* número de argumentos (todos g2c_gpr) */
  void* fn_arr;      /* si nargs > 6: g2c_gpr f(const uint64_t* args8); si no, NULL */
} g2c_func_entry;

/* Descripción de un fichero generado: lo que necesita el runtime para enlazarlo. */
typedef struct {
  const char* obj_name;          /* nombre del objeto GOAL ("math") */
  void (*link)(void);            /* resuelve símbolos y estáticos */
  const g2c_func_entry* funcs;   /* funciones traducidas, terminada en {0} */
  uint32_t seg_sizes[3];         /* tamaños de segmento del objeto de goalc (huella) */
} g2c_file_info;

/* ---- interfaz con el runtime de PC (modo mixto, librería dinámica) ----
   La librería (C generado + registro) exporta:
     const g2c_file_info* const* g2c_init(const g2c_host* host);
   que guarda el host y devuelve la lista de ficheros (terminada en NULL). */
typedef struct {
  uint8_t* mem;                                                    /* base memoria GOAL */
  uint32_t (*symbol)(const char* name);                            /* g2c_rt_symbol */
  uint32_t (*static_addr)(const char* obj, int seg, int off);      /* g2c_rt_static */
  uint64_t (*call)(uint32_t fn, int nargs, const uint64_t* args);  /* g2c_host_call */
  uint64_t* pp;                                                    /* g2c_pp_ptr */
} g2c_host;
typedef const g2c_file_info* const* (*g2c_init_fn)(const g2c_host* host);

/* ---- llamadas a funciones GOAL ----
   G2C_CALLn(f, a0, ..): f es la dirección GOAL del objeto función.
   - Por defecto (Vita, y en general todo el código en C): el objeto función guarda en su zona
     de código el puntero a la función C nativa (G2C_NATIVE_FN) y se llama directamente con
     la aridad del llamante.
   - G2C_PC_MIXED (runtime de PC con funciones x86 de goalc y C a la vez): la función llamada
     puede ser código x86, así que se pasa por el runtime (g2c_host_call). */
#ifdef G2C_PC_MIXED
uint64_t g2c_host_call(uint32_t fn, int nargs, const uint64_t* args);
#define G2C_ARGS(...) ((const uint64_t[]){__VA_ARGS__})
#define G2C_CALL0(f) ((g2c_gpr)g2c_host_call((uint32_t)(f), 0, G2C_ARGS(0)))
#define G2C_CALL1(f, a) ((g2c_gpr)g2c_host_call((uint32_t)(f), 1, G2C_ARGS(a)))
#define G2C_CALL2(f, a, b) ((g2c_gpr)g2c_host_call((uint32_t)(f), 2, G2C_ARGS(a, b)))
#define G2C_CALL3(f, a, b, c) ((g2c_gpr)g2c_host_call((uint32_t)(f), 3, G2C_ARGS(a, b, c)))
#define G2C_CALL4(f, a, b, c, d) ((g2c_gpr)g2c_host_call((uint32_t)(f), 4, G2C_ARGS(a, b, c, d)))
#define G2C_CALL5(f, a, b, c, d, e) \
  ((g2c_gpr)g2c_host_call((uint32_t)(f), 5, G2C_ARGS(a, b, c, d, e)))
#define G2C_CALL6(f, a, b, c, d, e, g) \
  ((g2c_gpr)g2c_host_call((uint32_t)(f), 6, G2C_ARGS(a, b, c, d, e, g)))
#define G2C_CALL7(f, a, b, c, d, e, g, h) \
  ((g2c_gpr)g2c_host_call((uint32_t)(f), 7, G2C_ARGS(a, b, c, d, e, g, h)))
#define G2C_CALL8(f, a, b, c, d, e, g, h, i) \
  ((g2c_gpr)g2c_host_call((uint32_t)(f), 8, G2C_ARGS(a, b, c, d, e, g, h, i)))
#else
#ifndef G2C_NATIVE_FN
#define G2C_NATIVE_FN(goal_addr) (*(void**)G2C_ADDR(goal_addr))
#endif
#define G2C_T g2c_gpr
#define G2C_CALL0(f) (((G2C_T(*)(void))G2C_NATIVE_FN(f))())
#define G2C_CALL1(f, a) (((G2C_T(*)(G2C_T))G2C_NATIVE_FN(f))(a))
#define G2C_CALL2(f, a, b) (((G2C_T(*)(G2C_T, G2C_T))G2C_NATIVE_FN(f))(a, b))
#define G2C_CALL3(f, a, b, c) (((G2C_T(*)(G2C_T, G2C_T, G2C_T))G2C_NATIVE_FN(f))(a, b, c))
#define G2C_CALL4(f, a, b, c, d) \
  (((G2C_T(*)(G2C_T, G2C_T, G2C_T, G2C_T))G2C_NATIVE_FN(f))(a, b, c, d))
#define G2C_CALL5(f, a, b, c, d, e) \
  (((G2C_T(*)(G2C_T, G2C_T, G2C_T, G2C_T, G2C_T))G2C_NATIVE_FN(f))(a, b, c, d, e))
#define G2C_CALL6(f, a, b, c, d, e, g) \
  (((G2C_T(*)(G2C_T, G2C_T, G2C_T, G2C_T, G2C_T, G2C_T))G2C_NATIVE_FN(f))(a, b, c, d, e, g))
#define G2C_CALL7(f, a, b, c, d, e, g, h)                                         \
  (((G2C_T(*)(G2C_T, G2C_T, G2C_T, G2C_T, G2C_T, G2C_T, G2C_T))G2C_NATIVE_FN(f))( \
      a, b, c, d, e, g, h))
#define G2C_CALL8(f, a, b, c, d, e, g, h, i)                                             \
  (((G2C_T(*)(G2C_T, G2C_T, G2C_T, G2C_T, G2C_T, G2C_T, G2C_T, G2C_T))G2C_NATIVE_FN(f))( \
      a, b, c, d, e, g, h, i))
#endif

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
/* Dirección real = base + dirección GOAL, sumada como uintptr_t (no como u32): en el PC, el
   código GOAL que corre en la pila x86 nativa pasa direcciones de pila "rsp - base" de 64
   bits, y goalc x86 las suma enteras a r15. En la Vita uintptr_t es de 32 bits y todo vive en
   la memoria GOAL, así que equivale a truncar a 32 bits.
   Se calcula como entero y se convierte a puntero (no como g2c_mem + a): una dirección GOAL
   puede apuntar fuera del bloque g2c_mem (p. ej. la pila de C de goal2c, g2c_stk), y con
   aritmética de punteros el compilador podría suponer que nunca toca otros objetos. */
#define G2C_ADDR(a) ((uint8_t*)((uintptr_t)g2c_mem + (uintptr_t)(a)))
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

/* ---- registros de 128 bits (clases VECTOR_FLOAT e INT_128 de goalc) ----
   Mismo tipo para las dos: los movimientos entre ellas copian los 128 bits. Semántica de cada
   operación = la de goalc x86, que a su vez sigue la MIPS de la PS2 (src1 = rs, src2 = rt).
   C portable por ahora; en la Fase 6 se pasará a NEON. */
typedef union {
  float f[4];
  uint32_t w[4];
  int32_t sw[4];
  uint64_t d[2];
  uint16_t h[8];
  int16_t sh[8];
  uint8_t b[16];
  int8_t sb[16];
} __attribute__((aligned(16))) g2c_q;

static inline g2c_q g2c_ld_q(g2c_gpr a) {
  g2c_q v;
  memcpy(&v, G2C_ADDR(a), 16);
  return v;
}
static inline void g2c_st_q(g2c_gpr a, g2c_q v) {
  memcpy(G2C_ADDR(a), &v, 16);
}
static inline g2c_q g2c_q_from_gpr(g2c_gpr x) { /* movq xmm, r64: parte alta a cero */
  g2c_q r;
  r.d[0] = (uint64_t)x;
  r.d[1] = 0;
  return r;
}
static inline g2c_q g2c_q_set_f0(g2c_q q, float x) { /* movss reg,reg: solo el elemento 0 */
  q.f[0] = x;
  return q;
}

/* float x4: vaddps & co. MAX/MIN como vmaxps/vminps: si hay NaN o son iguales, src2 */
#define G2C_QF_OP(name, expr)                         \
  static inline g2c_q name(g2c_q a, g2c_q b) {        \
    g2c_q r;                                          \
    for (int i = 0; i < 4; i++) r.f[i] = (expr);      \
    return r;                                         \
  }
G2C_QF_OP(g2c_qf_add, a.f[i] + b.f[i])
G2C_QF_OP(g2c_qf_sub, a.f[i] - b.f[i])
G2C_QF_OP(g2c_qf_mul, a.f[i] * b.f[i])
G2C_QF_OP(g2c_qf_div, a.f[i] / b.f[i])
G2C_QF_OP(g2c_qf_max, a.f[i] > b.f[i] ? a.f[i] : b.f[i])
G2C_QF_OP(g2c_qf_min, a.f[i] < b.f[i] ? a.f[i] : b.f[i])

static inline g2c_q g2c_q_xor(g2c_q a, g2c_q b) {
  g2c_q r;
  r.d[0] = a.d[0] ^ b.d[0];
  r.d[1] = a.d[1] ^ b.d[1];
  return r;
}
static inline g2c_q g2c_q_or(g2c_q a, g2c_q b) {
  g2c_q r;
  r.d[0] = a.d[0] | b.d[0];
  r.d[1] = a.d[1] | b.d[1];
  return r;
}
static inline g2c_q g2c_q_and(g2c_q a, g2c_q b) {
  g2c_q r;
  r.d[0] = a.d[0] & b.d[0];
  r.d[1] = a.d[1] & b.d[1];
  return r;
}
static inline g2c_q g2c_qf_sqrt(g2c_q a) { /* vsqrtps */
  g2c_q r;
  for (int i = 0; i < 4; i++) r.f[i] = __builtin_sqrtf(a.f[i]);
  return r;
}
static inline g2c_q g2c_qf_itof(g2c_q a) { /* vcvtdq2ps */
  g2c_q r;
  for (int i = 0; i < 4; i++) r.f[i] = (float)a.sw[i];
  return r;
}
static inline g2c_q g2c_qf_ftoi(g2c_q a) { /* vcvttps2dq: NaN/fuera de rango -> INT32_MIN */
  g2c_q r;
  for (int i = 0; i < 4; i++) {
    float x = a.f[i];
    r.sw[i] = (x >= -2147483648.0f && x < 2147483648.0f) ? (int32_t)x : INT32_MIN;
  }
  return r;
}
static inline g2c_q g2c_q_blend(g2c_q a, g2c_q b, unsigned mask) { /* vblendps: bit i -> b */
  g2c_q r;
  for (int i = 0; i < 4; i++) r.w[i] = ((mask >> i) & 1) ? b.w[i] : a.w[i];
  return r;
}
static inline g2c_q g2c_q_swizzle(g2c_q a, unsigned ctrl) { /* vshufps a, a, ctrl */
  g2c_q r;
  for (int i = 0; i < 4; i++) r.w[i] = a.w[(ctrl >> (2 * i)) & 3];
  return r;
}

/* enteros de 128 bits (MMI de la PS2): rd = op(rs = a, rt = b) */
static inline g2c_q g2c_pextlb(g2c_q a, g2c_q b) {
  g2c_q r;
  for (int i = 0; i < 8; i++) { r.b[2 * i] = b.b[i]; r.b[2 * i + 1] = a.b[i]; }
  return r;
}
static inline g2c_q g2c_pextub(g2c_q a, g2c_q b) {
  g2c_q r;
  for (int i = 0; i < 8; i++) { r.b[2 * i] = b.b[i + 8]; r.b[2 * i + 1] = a.b[i + 8]; }
  return r;
}
static inline g2c_q g2c_pextlh(g2c_q a, g2c_q b) {
  g2c_q r;
  for (int i = 0; i < 4; i++) { r.h[2 * i] = b.h[i]; r.h[2 * i + 1] = a.h[i]; }
  return r;
}
static inline g2c_q g2c_pextuh(g2c_q a, g2c_q b) {
  g2c_q r;
  for (int i = 0; i < 4; i++) { r.h[2 * i] = b.h[i + 4]; r.h[2 * i + 1] = a.h[i + 4]; }
  return r;
}
static inline g2c_q g2c_pextlw(g2c_q a, g2c_q b) {
  g2c_q r;
  r.w[0] = b.w[0]; r.w[1] = a.w[0]; r.w[2] = b.w[1]; r.w[3] = a.w[1];
  return r;
}
static inline g2c_q g2c_pextuw(g2c_q a, g2c_q b) {
  g2c_q r;
  r.w[0] = b.w[2]; r.w[1] = a.w[2]; r.w[2] = b.w[3]; r.w[3] = a.w[3];
  return r;
}
static inline g2c_q g2c_pcpyld(g2c_q a, g2c_q b) {
  g2c_q r;
  r.d[0] = b.d[0]; r.d[1] = a.d[0];
  return r;
}
static inline g2c_q g2c_pcpyud(g2c_q a, g2c_q b) {
  g2c_q r;
  r.d[0] = a.d[1]; r.d[1] = b.d[1];
  return r;
}
#define G2C_Q_LANES(name, field, n, expr)            \
  static inline g2c_q name(g2c_q a, g2c_q b) {       \
    g2c_q r;                                         \
    for (int i = 0; i < n; i++) r.field[i] = (expr); \
    return r;                                        \
  }
G2C_Q_LANES(g2c_pceqb, b, 16, a.b[i] == b.b[i] ? 0xff : 0)
G2C_Q_LANES(g2c_pceqh, h, 8, a.h[i] == b.h[i] ? 0xffff : 0)
G2C_Q_LANES(g2c_pceqw, w, 4, a.w[i] == b.w[i] ? 0xffffffffu : 0)
G2C_Q_LANES(g2c_pcgtb, b, 16, a.sb[i] > b.sb[i] ? 0xff : 0)
G2C_Q_LANES(g2c_pcgth, h, 8, a.sh[i] > b.sh[i] ? 0xffff : 0)
G2C_Q_LANES(g2c_pcgtw, w, 4, a.sw[i] > b.sw[i] ? 0xffffffffu : 0)
G2C_Q_LANES(g2c_psubw, w, 4, a.w[i] - b.w[i])
G2C_Q_LANES(g2c_paddb, b, 16, (uint8_t)(a.b[i] + b.b[i]))
static inline uint8_t g2c_sat_u8(int16_t x) {
  return x < 0 ? 0 : (x > 255 ? 255 : (uint8_t)x);
}
static inline g2c_q g2c_packuswb(g2c_q a, g2c_q b) { /* vpackuswb a, b */
  g2c_q r;
  for (int i = 0; i < 8; i++) { r.b[i] = g2c_sat_u8(a.sh[i]); r.b[i + 8] = g2c_sat_u8(b.sh[i]); }
  return r;
}
/* desplazamientos con inmediato (vpslld & co.): cuentas fuera de rango como en x86 */
static inline g2c_q g2c_pw_sll(g2c_q a, unsigned n) {
  g2c_q r;
  for (int i = 0; i < 4; i++) r.w[i] = n > 31 ? 0 : a.w[i] << n;
  return r;
}
static inline g2c_q g2c_pw_srl(g2c_q a, unsigned n) {
  g2c_q r;
  for (int i = 0; i < 4; i++) r.w[i] = n > 31 ? 0 : a.w[i] >> n;
  return r;
}
static inline g2c_q g2c_pw_sra(g2c_q a, unsigned n) {
  g2c_q r;
  for (int i = 0; i < 4; i++) r.sw[i] = a.sw[i] >> (n > 31 ? 31 : n);
  return r;
}
static inline g2c_q g2c_ph_sll(g2c_q a, unsigned n) {
  g2c_q r;
  for (int i = 0; i < 8; i++) r.h[i] = n > 15 ? 0 : (uint16_t)(a.h[i] << n);
  return r;
}
static inline g2c_q g2c_ph_srl(g2c_q a, unsigned n) {
  g2c_q r;
  for (int i = 0; i < 8; i++) r.h[i] = n > 15 ? 0 : (uint16_t)(a.h[i] >> n);
  return r;
}
static inline g2c_q g2c_psrldq(g2c_q a, unsigned n) { /* bytes hacia abajo */
  g2c_q r;
  for (unsigned i = 0; i < 16; i++) r.b[i] = (i + n < 16) ? a.b[i + n] : 0;
  return r;
}
static inline g2c_q g2c_pslldq(g2c_q a, unsigned n) { /* bytes hacia arriba */
  g2c_q r;
  for (unsigned i = 0; i < 16; i++) r.b[i] = (i >= n) ? a.b[i - n] : 0;
  return r;
}
static inline g2c_q g2c_pshuflw(g2c_q a, unsigned c) {
  g2c_q r = a;
  for (int i = 0; i < 4; i++) r.h[i] = a.h[(c >> (2 * i)) & 3];
  return r;
}
static inline g2c_q g2c_pshufhw(g2c_q a, unsigned c) {
  g2c_q r = a;
  for (int i = 0; i < 4; i++) r.h[4 + i] = a.h[4 + ((c >> (2 * i)) & 3)];
  return r;
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
