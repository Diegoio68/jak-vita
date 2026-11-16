// Jak Vita — benchmark goal2c (Fase 1, riesgo 3: coste de GPR u64 vs u32 en ARMv7).
//
// Enlaza dos veces el math.c que genera goal2c a partir de goal_src/jak1/engine/math/math.gc:
//   - variante u64 (la semánticamente correcta: los GPR de GOAL son de 64 bits)
//   - variante u32 (-DG2C_GPR32), solo para medir cuánto cuesta u64 en la Vita
// 1. Comprueba en ARM los resultados de ambas variantes contra la referencia de goalc x86
//    (out/goal2c/jak1/math_expected.h, generada por test/goalc/test_goal2c.cpp).
// 2. Mide ns por llamada de cada función en las dos variantes (llamada indirecta por puntero,
//    como serán las llamadas entre funciones GOAL).
// Pantalla: verde = todos los resultados u64 correctos, rojo = algún fallo. START sale.
// Log: ux0:data/jakvita/bench.txt

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vitaGL.h>

#include "math_expected.h"

#include "../src/log.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

using jakvita::log;

// ---- lo que el C generado necesita del runtime ----
#include "goal2c_rt.h"

extern "C" {

extern const g2c_func_entry g2c_math_funcs[];    // variante u64
extern const g2c_func_entry g2c_math_funcs32[];  // variante u32
void g2c_math_link(void);
void g2c_math_link32(void);

uint8_t* g2c_mem = nullptr;
static uint64_t g_pp_storage = 0;
uint64_t* g2c_pp_ptr = &g_pp_storage;
}

namespace {

// Memoria GOAL falsa. Cada variante (u64/u32) tiene su propio espacio de símbolos: si
// compartieran, rand-vu-int-range de una llamaría al rand-vu de la otra (ABI distinta).
struct SymSpace {
  uint32_t base;
  char names[64][64];
  int count;
};
SymSpace g_syms64{0x1000u, {}, 0};
SymSpace g_syms32{0x2000u, {}, 0};

uint32_t sym_lookup(SymSpace& s, const char* name) {
  for (int i = 0; i < s.count; i++) {
    if (strcmp(s.names[i], name) == 0) {
      return s.base + 16u * (uint32_t)i;
    }
  }
  strncpy(s.names[s.count], name, 63);
  return s.base + 16u * (uint32_t)s.count++;
}

uint32_t g_bump = 0x10000u;
uint32_t goal_alloc(uint32_t size) {
  uint32_t addr = (g_bump + 15u) & ~15u;
  g_bump = addr + size;
  return addr;
}

// pc-rand determinista: la misma secuencia (contador * 7919) que usa la referencia de goalc
uint64_t g_pc_rand_counter = 0;
uint64_t fake_pc_rand64() {
  g_pc_rand_counter++;
  return g_pc_rand_counter * 7919u;
}
uint32_t fake_pc_rand32() {
  g_pc_rand_counter++;
  return (uint32_t)(g_pc_rand_counter * 7919u);
}

}  // namespace

extern "C" {
uint32_t g2c_rt_symbol(const char* name) {
  return sym_lookup(g_syms64, name);
}
uint32_t g2c_rt_symbol32(const char* name) {
  return sym_lookup(g_syms32, name);
}
// el bench no tiene los datos de los objetos .go: direcciones únicas sin contenido
uint32_t g2c_rt_static(const char*, int, int) {
  return goal_alloc(64);
}
uint32_t g2c_rt_static32(const char*, int, int) {
  return goal_alloc(64);
}
}

namespace {

constexpr int kMemSize = 1 << 20;
constexpr int kIters = 1000000;
// Cada ejecución mide todas las funciones una vez, alternando u64 y u32; se informa de la
// mediana de kRuns ejecuciones (y del rango), no del mejor tiempo.
constexpr int kRuns = 7;

// Lo que haría el código top-level de GOAL al cargar el fichero: objeto función con el
// puntero nativo en su zona de código (convención G2C_NATIVE_FN) y guardado en su símbolo.
void define_function(SymSpace& space, const char* name, void* native) {
  uint32_t fobj = goal_alloc(16);
  memcpy(g2c_mem + fobj, &native, sizeof(native));
  uint32_t value = fobj;
  memcpy(g2c_mem + sym_lookup(space, name), &value, 4);
}

void define_all(SymSpace& space, const g2c_func_entry* table, void* pc_rand) {
  for (auto* e = table; e->name; e++) {
    define_function(space, e->name, e->fn);
  }
  define_function(space, "pc-rand", pc_rand);
}

const g2c_func_entry* find(const g2c_func_entry* table, const char* name) {
  for (auto* e = table; e->name; e++) {
    if (strcmp(e->name, name) == 0) {
      return e;
    }
  }
  return nullptr;
}

template <typename R>
R call(const g2c_func_entry* e, const R* a) {
  switch (e->nargs) {
    case 0:
      return ((R (*)())e->fn)();
    case 1:
      return ((R (*)(R))e->fn)(a[0]);
    case 2:
      return ((R (*)(R, R))e->fn)(a[0], a[1]);
    case 3:
      return ((R (*)(R, R, R))e->fn)(a[0], a[1], a[2]);
    case 4:
      return ((R (*)(R, R, R, R))e->fn)(a[0], a[1], a[2], a[3]);
    case 5:
      return ((R (*)(R, R, R, R, R))e->fn)(a[0], a[1], a[2], a[3], a[4]);
    default:
      return ((R (*)(R, R, R, R, R, R, R, R))e->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  }
}

// Compara un resultado con la referencia de goalc. is32: la variante devuelve 32 bits.
bool check(const char* kind, uint64_t raw, uint64_t expected, bool is32) {
  SymSpace& space = is32 ? g_syms32 : g_syms64;
  const uint64_t sym_t = sym_lookup(space, "#t");
  const uint64_t sym_f = sym_lookup(space, "#f");
  if (strcmp(kind, "float") == 0) {
    return (raw & 0xffffffffu) == expected;
  }
  if (strcmp(kind, "bool") == 0) {
    return (raw == sym_t && expected == 1) || (raw == sym_f && expected == 0);
  }
  if (is32) {
    raw = (uint64_t)(int64_t)(int32_t)(uint32_t)raw;
  }
  return raw == expected;
}

template <typename R>
int check_all(const g2c_func_entry* table, bool is32, const char* label) {
  int bad = 0;
  g_pc_rand_counter = 0;  // la referencia empieza la secuencia de pc-rand desde 0
  const int n = sizeof(g2c_math_expected) / sizeof(g2c_math_expected[0]);
  for (int i = 0; i < n; i++) {
    auto& c = g2c_math_expected[i];
    auto* e = find(table, c.fn);
    if (!e) {
      log("ERROR [%s] función no encontrada: %s", label, c.fn);
      bad++;
      continue;
    }
    R args[8];
    for (int j = 0; j < 8; j++) {
      args[j] = (R)c.args[j];
    }
    uint64_t got = (uint64_t)call<R>(e, args);
    if (!check(c.kind, got, c.expected, is32)) {
      bad++;
      // u64 es la variante real: un fallo ahí es un ERROR. En u32 solo informa.
      log("%s [%s] caso %d %s: esperado 0x%llx, obtenido 0x%llx", is32 ? "aviso" : "ERROR", label,
          i, c.fn, (unsigned long long)c.expected, (unsigned long long)got);
    }
  }
  log("[%s] comprobación: %d/%d correctos", label, n - bad, n);
  return bad;
}

volatile uint64_t g_sink;

// ns por llamada de una función, con las entradas de sus casos de prueba en rotación.
template <typename R>
double bench_fn(const g2c_func_entry* table, const char* fn) {
  auto* e = find(table, fn);
  if (!e) {
    return -1;
  }
  R inputs[16][8];
  int ninputs = 0;
  const int n = sizeof(g2c_math_expected) / sizeof(g2c_math_expected[0]);
  for (int i = 0; i < n && ninputs < 16; i++) {
    if (strcmp(g2c_math_expected[i].fn, fn) == 0) {
      for (int j = 0; j < 8; j++) {
        inputs[ninputs][j] = (R)g2c_math_expected[i].args[j];
      }
      ninputs++;
    }
  }
  if (ninputs == 0) {
    return -1;
  }
  {
    uint64_t acc = 0;
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    // Bucle por aridad: llamada indirecta con la firma exacta, sin switch ni división
    // dentro del bucle (el Cortex-A9 no tiene división entera por hardware).
    int k = 0;
#define G2C_BENCH_LOOP(CALL)         \
  for (int i = 0; i < kIters; i++) { \
    const R* a = inputs[k];          \
    acc ^= (uint64_t)(CALL);         \
    if (++k == ninputs)              \
      k = 0;                         \
  }
    switch (e->nargs) {
      case 0: {
        auto f = (R (*)())e->fn;
        G2C_BENCH_LOOP(((void)a, f()));
      } break;
      case 1: {
        auto f = (R (*)(R))e->fn;
        G2C_BENCH_LOOP(f(a[0]));
      } break;
      case 2: {
        auto f = (R (*)(R, R))e->fn;
        G2C_BENCH_LOOP(f(a[0], a[1]));
      } break;
      case 3: {
        auto f = (R (*)(R, R, R))e->fn;
        G2C_BENCH_LOOP(f(a[0], a[1], a[2]));
      } break;
      case 5: {
        auto f = (R (*)(R, R, R, R, R))e->fn;
        G2C_BENCH_LOOP(f(a[0], a[1], a[2], a[3], a[4]));
      } break;
      default:
        log("ERROR aridad %d no soportada en el benchmark (%s)", e->nargs, fn);
        return -1;
    }
#undef G2C_BENCH_LOOP
    SceUInt64 t1 = sceKernelGetProcessTimeWide();
    g_sink = acc;
    return (double)(t1 - t0) * 1000.0 / kIters;
  }
  return -1;
}

}  // namespace

int main() {
  jakvita::log_init("ux0:data/jakvita/bench.txt");
  log("goal2c bench (Fase 1) - build %s %s", __DATE__, __TIME__);
  log("CPU %d MHz, bus %d MHz", scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency());

  constexpr int MB = 1024 * 1024;
  vglInitWithCustomSizes(8 * MB, 960, 544, 16 * MB, 16 * MB, 4 * MB, 2 * MB,
                         SCE_GXM_MULTISAMPLE_NONE);

  g2c_mem = new uint8_t[kMemSize]();
  for (SymSpace* sp : {&g_syms64, &g_syms32}) {
    sym_lookup(*sp, "#f");
    sym_lookup(*sp, "#t");
  }
  g2c_math_link();
  g2c_math_link32();
  define_all(g_syms64, g2c_math_funcs, (void*)fake_pc_rand64);
  define_all(g_syms32, g2c_math_funcs32, (void*)fake_pc_rand32);

  // 1) corrección en ARM
  int bad64 = check_all<uint64_t>(g2c_math_funcs, false, "u64");
  int bad32 = check_all<uint32_t>(g2c_math_funcs32, true, "u32");

  // 2) rendimiento
  // rand-vu: 1 llamada a pc-rand; rand-vu-int-range: cadena de 3 llamadas GOAL->GOAL->GOAL->C
  const char* fns[] = {"truncate",   "log2",    "seekl",
                       "seek",       "lerp",    "lerp-clamp",
                       "lerp-scale", "rand-vu", "rand-vu-int-range"};
  constexpr int kFns = sizeof(fns) / sizeof(fns[0]);
  log("benchmark: %d llamadas por medida, %d ejecuciones alternando u64/u32; ns por llamada "
      "(mediana [min-max])",
      kIters, kRuns);
  static double t64[kFns][kRuns], t32[kFns][kRuns];
  for (int run = 0; run < kRuns; run++) {
    for (int f = 0; f < kFns; f++) {
      // alterna el orden para repartir por igual la deriva (temperatura, reloj)
      if (run % 2 == 0) {
        t64[f][run] = bench_fn<uint64_t>(g2c_math_funcs, fns[f]);
        t32[f][run] = bench_fn<uint32_t>(g2c_math_funcs32, fns[f]);
      } else {
        t32[f][run] = bench_fn<uint32_t>(g2c_math_funcs32, fns[f]);
        t64[f][run] = bench_fn<uint64_t>(g2c_math_funcs, fns[f]);
      }
    }
    log("  ejecución %d/%d terminada", run + 1, kRuns);
  }
  auto median = [](double* v, int n, double* lo, double* hi) {
    double tmp[kRuns];
    for (int i = 0; i < n; i++) {
      tmp[i] = v[i];
    }
    for (int i = 1; i < n; i++) {  // ordenación por inserción (n pequeño)
      double x = tmp[i];
      int j = i - 1;
      while (j >= 0 && tmp[j] > x) {
        tmp[j + 1] = tmp[j];
        j--;
      }
      tmp[j + 1] = x;
    }
    *lo = tmp[0];
    *hi = tmp[n - 1];
    return (n % 2) ? tmp[n / 2] : 0.5 * (tmp[n / 2 - 1] + tmp[n / 2]);
  };
  log("%-18s %22s %22s %7s", "funcion", "u64", "u32", "u64/u32");
  double sum64 = 0, sum32 = 0;
  for (int f = 0; f < kFns; f++) {
    double lo64, hi64, lo32, hi32;
    double m64 = median(t64[f], kRuns, &lo64, &hi64);
    double m32 = median(t32[f], kRuns, &lo32, &hi32);
    sum64 += m64;
    sum32 += m32;
    log("%-18s %7.1f [%5.1f-%5.1f] %7.1f [%5.1f-%5.1f] %7.2f", fns[f], m64, lo64, hi64, m32, lo32,
        hi32, m32 > 0 ? m64 / m32 : 0.0);
  }
  log("%-18s %7.1f %14s %7.1f %14s %7.2f", "TOTAL (medianas)", sum64, "", sum32, "", sum64 / sum32);
  log("RESULTADO: u64 %s (%d fallos), u32 %d diferencias", bad64 == 0 ? "OK" : "FALLA", bad64,
      bad32);

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
  for (;;) {
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) {
      break;
    }
    if (bad64 == 0) {
      glClearColor(0.1f, 0.7f, 0.2f, 1.0f);  // verde: todo correcto
    } else {
      glClearColor(0.8f, 0.1f, 0.1f, 1.0f);  // rojo: algún resultado incorrecto
    }
    glClear(GL_COLOR_BUFFER_BIT);
    vglSwapBuffers(GL_FALSE);
  }
  log("Fin");
  jakvita::log_shutdown();
  sceKernelExitProcess(0);
  return 0;
}
