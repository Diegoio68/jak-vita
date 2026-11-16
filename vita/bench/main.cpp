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
#include <vitaGL.h>

#include "math_expected.h"

#include "../src/log.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

using jakvita::log;

// ---- lo que el C generado necesita del runtime ----
extern "C" {
typedef struct {
  const char* name;
  void* fn;
  int nargs;
} g2c_func_entry;

extern const g2c_func_entry g2c_math_funcs[];    // variante u64
extern const g2c_func_entry g2c_math_funcs32[];  // variante u32
void g2c_math_link(void);
void g2c_math_link32(void);

uint8_t* g2c_mem = nullptr;

// Memoria GOAL falsa con los símbolos en 0x1000 + 16*i (como el arnés de PC).
static char g_sym_names[64][64];
static int g_sym_count = 0;
uint32_t g2c_rt_symbol(const char* name) {
  for (int i = 0; i < g_sym_count; i++) {
    if (strcmp(g_sym_names[i], name) == 0) {
      return 0x1000u + 16u * (uint32_t)i;
    }
  }
  strncpy(g_sym_names[g_sym_count], name, 63);
  return 0x1000u + 16u * (uint32_t)g_sym_count++;
}
}

namespace {

constexpr int kMemSize = 1 << 20;
constexpr int kIters = 1000000;
constexpr int kRepeats = 3;

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
  const uint64_t sym_t = g2c_rt_symbol("#t");
  const uint64_t sym_f = g2c_rt_symbol("#f");
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
  double best = 1e30;
  for (int rep = 0; rep < kRepeats; rep++) {
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
    double ns = (double)(t1 - t0) * 1000.0 / kIters;
    if (ns < best) {
      best = ns;
    }
  }
  return best;
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
  g2c_rt_symbol("#f");
  g2c_rt_symbol("#t");
  g2c_math_link();
  g2c_math_link32();

  // 1) corrección en ARM
  int bad64 = check_all<uint64_t>(g2c_math_funcs, false, "u64");
  int bad32 = check_all<uint32_t>(g2c_math_funcs32, true, "u32");

  // 2) rendimiento
  const char* fns[] = {"truncate", "log2", "seekl", "seek", "lerp", "lerp-clamp", "lerp-scale"};
  log("benchmark: %d llamadas x %d repeticiones (mejor tiempo), ns por llamada", kIters, kRepeats);
  log("%-12s %8s %8s %7s", "funcion", "u64", "u32", "u64/u32");
  double sum64 = 0, sum32 = 0;
  for (auto* fn : fns) {
    double t64 = bench_fn<uint64_t>(g2c_math_funcs, fn);
    double t32 = bench_fn<uint32_t>(g2c_math_funcs32, fn);
    sum64 += t64;
    sum32 += t32;
    log("%-12s %8.1f %8.1f %7.2f", fn, t64, t32, t32 > 0 ? t64 / t32 : 0.0);
  }
  log("%-12s %8.1f %8.1f %7.2f", "TOTAL", sum64, sum32, sum64 / sum32);
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
