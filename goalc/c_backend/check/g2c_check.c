/*!
 * @file g2c_check.c
 * goal2c (Jak Vita): comprobación del C generado contra goalc, en PC y en ARM (Vita).
 *
 * Reproduce los casos grabados en el runtime de PC (test/goal2c/arm/<lote>_arm.gc, GOAL2C_REC):
 * 1. Carga la imagen de la memoria GOAL (<lote>.img.z, zlib) en g2c_mem.
 * 2. Enlaza el C de todos los ficheros validados (tabla g2c_check_files, generada por
 *    goalc/c_backend/tools/build_check.py): símbolos y estáticos se buscan en la imagen.
 * 3. Parchea cada objeto función de la imagen que tiene versión en C: puntero nativo + marca,
 *    igual que hará el cargador de la Vita. Las llamadas GOAL (símbolos, métodos, lambdas)
 *    llegan así al C; lo que no tiene C conserva el código x86 y no lleva la marca.
 * 4. Por cada caso: arena de antes, llamada a la función (por su símbolo, como GOAL) con los
 *    argumentos grabados, y compara el retorno y la arena de después con lo grabado en x86.
 *    Las NaN se comparan sin signo (la NaN por defecto de ARM es 0x7fc00000, la de x86
 *    0xffc00000; la PS2 no tiene NaN).
 * Un caso que llama a algo sin C o lee fuera de la imagen es INVALIDO (no cuenta como
 * diferencia; hay que cambiar el caso).
 *
 * Formato de <lote>.rec (little endian), lo escribe g2c_rec en game/goal2c/goal2c_jak1.cpp:
 *   "G2CR" u32 versión(1) u32 tamaño_imagen u32 s7 u32 SymbolTable2 u32 LastSymbol
 *   u32 desplazamiento_info_símbolo u32 tipo_function u32 arena u32 tamaño_arena
 *   u32 nobjs { str nombre, u32 seg0_off seg0_size seg1_off seg1_size seg2_off seg2_size }
 *   { "CASE" str etiqueta str función u32 nargs u32 flags u64 args[8] u64 retorno
 *     u8 arena_antes[tamaño_arena] u8 arena_después[tamaño_arena] } ... "END!"
 *   str = u16 longitud + bytes.
 *
 * Uso en PC:  g2c_check <carpeta con .rec e .img.z> [salida.txt]
 */
#include "g2c_check.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "goal2c_rt.h"

extern const g2c_file_info* const g2c_check_files[];

uint8_t* g2c_mem;
uint64_t* g2c_pp_ptr;
static uint64_t g_pp_dummy;

/* ---- salida ---- */
static FILE* g_out;
static void say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  if (g_out) {
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
    fflush(g_out);
  }
}

/* ---- imagen y accesos ---- */
static uint32_t g_image_size;
static uintptr_t g_stack_lo, g_stack_hi;
static int g_invalid; /* el caso actual hizo algo no reproducible */
static char g_invalid_why[160];
static uint8_t g_trap[256] __attribute__((aligned(16)));

static void mark_invalid(const char* why, uintptr_t a) {
  if (!g_invalid) {
    snprintf(g_invalid_why, sizeof(g_invalid_why), "%s 0x%llx", why, (unsigned long long)a);
  }
  g_invalid = 1;
}

uint8_t* g2c_check_addr(uintptr_t a) {
  if (a < g_image_size) {
    return g2c_mem + a;
  }
  uintptr_t host = (uintptr_t)g2c_mem + a; /* variables de pila de GOAL: pila de C */
  if (host >= g_stack_lo && host < g_stack_hi) {
    return (uint8_t*)host;
  }
  mark_invalid("acceso fuera de la imagen grabada:", a);
  memset(g_trap, 0, sizeof(g_trap));
  return g_trap;
}

static const char* symbol_holding(uint32_t value);
static uint64_t bad_call(void) {
  return 0;
}

/* Funciones parcheadas: el objeto función guarda (4 bytes, como en la Vita) un índice en esta
   tabla, válido solo si la entrada apunta a esa misma dirección GOAL. */
typedef struct {
  uint32_t addr;
  void* fn;
} patch_entry;
static patch_entry* g_patches;
static uint32_t g_npatches, g_cap_patches;

void* g2c_check_fn(uintptr_t a) {
  if (a + 4 <= g_image_size) {
    uint32_t i;
    memcpy(&i, g2c_mem + a, 4);
    if (i < g_npatches && g_patches[i].addr == a) {
      return g_patches[i].fn;
    }
  }
  if (!g_invalid) {
    mark_invalid("llamada a una función sin C (x86 o kernel) en", a);
    const char* who = symbol_holding(a);
    if (who) {
      size_t n = strlen(g_invalid_why);
      snprintf(g_invalid_why + n, sizeof(g_invalid_why) - n, " (%s)", who);
    }
  }
  return (void*)bad_call;
}

/* ---- lectura del .rec ---- */
typedef struct {
  const uint8_t* p;
  const uint8_t* end;
  int bad;
} reader;
static uint32_t rd_u32(reader* r) {
  uint32_t v = 0;
  if (r->p + 4 > r->end) {
    r->bad = 1;
    return 0;
  }
  memcpy(&v, r->p, 4);
  r->p += 4;
  return v;
}
static uint64_t rd_u64(reader* r) {
  uint64_t v = 0;
  if (r->p + 8 > r->end) {
    r->bad = 1;
    return 0;
  }
  memcpy(&v, r->p, 8);
  r->p += 8;
  return v;
}
static void rd_str(reader* r, char* out, size_t cap) {
  uint16_t n = 0;
  if (r->p + 2 > r->end) {
    r->bad = 1;
    out[0] = 0;
    return;
  }
  memcpy(&n, r->p, 2);
  r->p += 2;
  if (r->p + n > r->end) {
    r->bad = 1;
    out[0] = 0;
    return;
  }
  size_t k = n < cap - 1 ? n : cap - 1;
  memcpy(out, r->p, k);
  out[k] = 0;
  r->p += n;
}
static const uint8_t* rd_bytes(reader* r, uint32_t n) {
  if (r->p + n > r->end) {
    r->bad = 1;
    return NULL;
  }
  const uint8_t* q = r->p;
  r->p += n;
  return q;
}

static uint8_t* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* buf = malloc((size_t)n + 1);
  if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  *size = (size_t)n;
  return buf;
}

/* ---- símbolos y objetos de la imagen ---- */
typedef struct {
  const char* name;
  uint32_t addr;
} sym_entry;
static sym_entry* g_syms;
static int g_nsyms;
typedef struct {
  char name[64];
  uint32_t seg[6];
} obj_entry;
static obj_entry* g_objs;
static uint32_t g_nobjs;
static uint32_t g_function_type, g_s7;

static int sym_cmp(const void* a, const void* b) {
  return strcmp(((const sym_entry*)a)->name, ((const sym_entry*)b)->name);
}

static void index_symbols(uint32_t table2, uint32_t last, uint32_t info_off) {
  g_syms = malloc(sizeof(sym_entry) * ((last - table2) / 8 + 1));
  g_nsyms = 0;
  for (uint32_t s = table2; s < last; s += 8) {
    uint32_t str;
    memcpy(&str, g2c_mem + s + info_off + 4, 4); /* SymInfo { hash, str } */
    if (!str || str + 4 >= g_image_size) {
      continue;
    }
    g_syms[g_nsyms].name = (const char*)(g2c_mem + str + 4); /* String: u32 len + datos */
    g_syms[g_nsyms].addr = s;
    g_nsyms++;
  }
  qsort(g_syms, (size_t)g_nsyms, sizeof(sym_entry), sym_cmp);
}

/* nombre del símbolo cuyo valor es value (para los mensajes de diagnóstico) */
static const char* symbol_holding(uint32_t value) {
  for (int i = 0; i < g_nsyms; i++) {
    uint32_t v;
    memcpy(&v, g2c_mem + g_syms[i].addr, 4);
    if (v == value) {
      return g_syms[i].name;
    }
  }
  return NULL;
}

static uint32_t find_symbol(const char* name) {
  sym_entry key = {name, 0};
  sym_entry* e = bsearch(&key, g_syms, (size_t)g_nsyms, sizeof(sym_entry), sym_cmp);
  return e ? e->addr : 0;
}

static int g_missing_syms;
uint32_t g2c_rt_symbol(const char* name) {
  uint32_t a = find_symbol(name);
  if (!a) {
    g_missing_syms++;
    return g_s7; /* #f; un caso que lo use dará otro resultado o será inválido */
  }
  return a;
}

static const obj_entry* find_obj(const char* name) {
  for (uint32_t i = 0; i < g_nobjs; i++) {
    if (!strcmp(g_objs[i].name, name)) {
      return &g_objs[i];
    }
  }
  return NULL;
}

uint32_t g2c_rt_static(const char* obj_name, int seg, int offset) {
  const obj_entry* o = find_obj(obj_name);
  if (!o || seg < 0 || seg > 2) {
    return 0;
  }
  return o->seg[2 * seg] + (uint32_t)offset;
}

/* Enlaza todos los ficheros y parchea sus objetos función en la imagen. */
static void link_and_patch(int* nfiles, int* npatched, int* nmissing_obj) {
  *nfiles = *npatched = *nmissing_obj = 0;
  g_npatches = 0;
  for (const g2c_file_info* const* f = g2c_check_files; *f; f++) {
    const g2c_file_info* info = *f;
    const obj_entry* o = find_obj(info->obj_name);
    (*nfiles)++;
    if (!o) {
      (*nmissing_obj)++;
      continue;
    }
    info->link();
    for (const g2c_func_entry* e = info->funcs; e->name; e++) {
      if (e->seg < 0 || e->seg > 2 || e->length < 4) {
        continue;
      }
      uint32_t seg_off = o->seg[2 * e->seg], seg_size = o->seg[2 * e->seg + 1];
      if (seg_size == 0 || (uint32_t)e->offset + 12 > seg_size) {
        continue; /* segmento no cargado (depuración) */
      }
      if (seg_off < 4 || (uint64_t)seg_off + (uint32_t)e->offset + 12 > g_image_size) {
        continue; /* fuera de la imagen (objeto de un heap de nivel o de depuración) */
      }
      uint8_t* code = g2c_mem + seg_off + (uint32_t)e->offset;
      uint32_t tag;
      memcpy(&tag, code - 4, 4);
      if (tag != g_function_type) {
        continue; /* no es el objeto función que esperamos: no tocar */
      }
      if (g_npatches == g_cap_patches) {
        g_cap_patches = g_cap_patches ? 2 * g_cap_patches : 4096;
        g_patches = realloc(g_patches, g_cap_patches * sizeof(patch_entry));
      }
      uint32_t idx = g_npatches++;
      g_patches[idx].addr = (uint32_t)(code - g2c_mem);
      g_patches[idx].fn = e->fn;
      memcpy(code, &idx, 4);
      (*npatched)++;
    }
  }
}

/* ---- comparación ---- */
static int is_nan32(uint32_t w) {
  return (w & 0x7f800000u) == 0x7f800000u && (w & 0x007fffffu) != 0;
}
static int g_nan_equiv; /* comparaciones que solo coinciden ignorando el signo de las NaN */
static int same_word(uint32_t a, uint32_t b) {
  if (a == b) {
    return 1;
  }
  if (is_nan32(a) && is_nan32(b)) {
    g_nan_equiv++;
    return 1;
  }
  return 0;
}
static int same_ret(uint64_t a, uint64_t b) {
  if (a == b) {
    return 1;
  }
  uint32_t la = (uint32_t)a, lb = (uint32_t)b;
  if (is_nan32(la) && is_nan32(lb)) {
    /* float devuelto en un GPR: 32 bits con el signo extendido */
    uint32_t ha = (uint32_t)(a >> 32), hb = (uint32_t)(b >> 32);
    int ext_a = ha == ((la & 0x80000000u) ? 0xffffffffu : 0);
    int ext_b = hb == ((lb & 0x80000000u) ? 0xffffffffu : 0);
    if (ext_a && ext_b) {
      g_nan_equiv++;
      return 1;
    }
  }
  return 0;
}

typedef uint64_t (*fn0)(void);
typedef uint64_t (*fn1)(g2c_gpr);
typedef uint64_t (*fn2)(g2c_gpr, g2c_gpr);
typedef uint64_t (*fn3)(g2c_gpr, g2c_gpr, g2c_gpr);
typedef uint64_t (*fn4)(g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr);
typedef uint64_t (*fn5)(g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr);
typedef uint64_t (*fn6)(g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr);
typedef uint64_t (*fn7)(g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr);
typedef uint64_t (*fn8)(g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr);

static uint64_t call_native(void* f, uint32_t n, const uint64_t* a) {
  switch (n) {
    case 0:
      return ((fn0)f)();
    case 1:
      return ((fn1)f)(a[0]);
    case 2:
      return ((fn2)f)(a[0], a[1]);
    case 3:
      return ((fn3)f)(a[0], a[1], a[2]);
    case 4:
      return ((fn4)f)(a[0], a[1], a[2], a[3]);
    case 5:
      return ((fn5)f)(a[0], a[1], a[2], a[3], a[4]);
    case 6:
      return ((fn6)f)(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 7:
      return ((fn7)f)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    default:
      return ((fn8)f)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  }
}

/* Ejecuta los casos de un lote. Devuelve el número de casos que difieren. */
static int run_lot(const char* dir, const char* lot, int* n_ok, int* n_bad, int* n_inv) {
  char path[512];
  size_t rec_size = 0, z_size = 0;
  snprintf(path, sizeof(path), "%s/%s.rec", dir, lot);
  uint8_t* rec = read_file(path, &rec_size);
  snprintf(path, sizeof(path), "%s/%s.img.z", dir, lot);
  uint8_t* z = read_file(path, &z_size);
  if (!rec || !z) {
    say("ERROR %s: no se pueden leer %s.rec / %s.img.z en %s\n", lot, lot, lot, dir);
    return -1;
  }
  reader r = {rec, rec + rec_size, 0};
  if (memcmp(rd_bytes(&r, 4), "G2CR", 4) != 0 || rd_u32(&r) != 1) {
    say("ERROR %s: .rec con formato desconocido\n", lot);
    return -1;
  }
  g_image_size = rd_u32(&r);
  g_s7 = rd_u32(&r);
  uint32_t table2 = rd_u32(&r), last = rd_u32(&r), info_off = rd_u32(&r);
  g_function_type = rd_u32(&r);
  uint32_t arena = rd_u32(&r), arena_size = rd_u32(&r);
  g_nobjs = rd_u32(&r);
  g_objs = calloc(g_nobjs, sizeof(obj_entry));
  for (uint32_t i = 0; i < g_nobjs; i++) {
    rd_str(&r, g_objs[i].name, sizeof(g_objs[i].name));
    for (int k = 0; k < 6; k++) {
      g_objs[i].seg[k] = rd_u32(&r);
    }
  }

  /* imagen: 1 MB de margen al final, a cero */
  free(g2c_mem);
  g2c_mem = malloc((size_t)g_image_size + (1 << 20));
  if (!g2c_mem) {
    say("ERROR %s: sin memoria para la imagen (%u MB)\n", lot, g_image_size >> 20);
    return -1;
  }
  memset(g2c_mem + g_image_size, 0, 1 << 20);
  uLongf dest_len = g_image_size;
  if (uncompress(g2c_mem, &dest_len, z, (uLong)z_size) != Z_OK || dest_len != g_image_size) {
    say("ERROR %s: no se pudo descomprimir la imagen\n", lot);
    return -1;
  }
  free(z);
  index_symbols(table2, last, info_off);
  int nfiles, npatched, nmissing;
  g_missing_syms = 0;
  link_and_patch(&nfiles, &npatched, &nmissing);
  say("%s: imagen %u MB, %d símbolos, %d ficheros C (%d sin objeto en la imagen), "
      "%d funciones parcheadas, %d símbolos que no existen\n",
      lot, g_image_size >> 20, g_nsyms, nfiles, nmissing, npatched, g_missing_syms);

  *n_ok = *n_bad = *n_inv = 0;
  uint8_t* after = malloc(arena_size);
  for (;;) {
    const uint8_t* tag = rd_bytes(&r, 4);
    if (!tag || !memcmp(tag, "END!", 4)) {
      break;
    }
    if (memcmp(tag, "CASE", 4)) {
      say("ERROR %s: .rec corrupto\n", lot);
      break;
    }
    char label[128], fname[128];
    rd_str(&r, label, sizeof(label));
    rd_str(&r, fname, sizeof(fname));
    uint32_t nargs = rd_u32(&r), flags = rd_u32(&r);
    uint64_t args[8];
    for (int i = 0; i < 8; i++) {
      args[i] = rd_u64(&r);
    }
    uint64_t want_ret = rd_u64(&r);
    const uint8_t* before = rd_bytes(&r, arena_size);
    const uint8_t* want_after = rd_bytes(&r, arena_size);
    if (r.bad) {
      say("ERROR %s: .rec truncado\n", lot);
      break;
    }
    memcpy(g2c_mem + arena, before, arena_size);
    g_invalid = 0;
    uint32_t sym = find_symbol(fname);
    uint64_t got = 0;
    if (!sym) {
      mark_invalid("símbolo de la función no encontrado", 0);
    } else {
      uint32_t fobj;
      memcpy(&fobj, g2c_mem + sym, 4);
      void* f = g2c_check_fn(fobj);
      if (!g_invalid) {
        got = call_native(f, nargs > 8 ? 8 : nargs, args);
      }
    }
    if (g_invalid) {
      (*n_inv)++;
      say("INVALIDO %s (%s): %s\n", label, fname, g_invalid_why);
      continue;
    }
    memcpy(after, g2c_mem + arena, arena_size);
    int diff_words = 0;
    uint32_t first_diff = 0, want_w = 0, got_w = 0;
    for (uint32_t off = 0; off + 4 <= arena_size; off += 4) {
      uint32_t a, b;
      memcpy(&a, want_after + off, 4);
      memcpy(&b, after + off, 4);
      if (!same_word(a, b)) {
        if (!diff_words) {
          first_diff = off;
          want_w = a;
          got_w = b;
        }
        diff_words++;
      }
    }
    int ret_ok = (flags & 1) || same_ret(want_ret, got);
    if (ret_ok && !diff_words) {
      (*n_ok)++;
      say("OK %s\n", label);
    } else {
      (*n_bad)++;
      say("DIFIERE %s (%s): retorno goalc=0x%llx arm=0x%llx%s; %d palabras de la arena "
          "distintas",
          label, fname, (unsigned long long)want_ret, (unsigned long long)got,
          ret_ok ? " (igual)" : "", diff_words);
      if (diff_words) {
        say(" (primera +0x%x: goalc=0x%08x arm=0x%08x)", first_diff, want_w, got_w);
      }
      say("\n");
    }
  }
  free(after);
  free(rec);
  free(g_syms);
  free(g_objs);
  return *n_bad;
}

/* Recorre los lotes de lots (lista terminada en NULL). Devuelve 0 si ningún caso difiere. */
int g2c_check_main(const char* dir, const char* const* lots, FILE* out) {
  volatile char probe = 0;
  g_stack_hi = (uintptr_t)&probe + (64 << 10);
  g_stack_lo = (uintptr_t)&probe - (4 << 20);
  g_out = out;
  g2c_pp_ptr = &g_pp_dummy;
  int total_ok = 0, total_bad = 0, total_inv = 0, errors = 0;
  for (const char* const* l = lots; *l; l++) {
    int ok, bad, inv;
    g_nan_equiv = 0;
    if (run_lot(dir, *l, &ok, &bad, &inv) < 0) {
      errors++;
      continue;
    }
    say("%s: %d OK, %d DIFIEREN, %d INVALIDOS (%d coincidencias solo ignorando el signo de "
        "NaN)\n",
        *l, ok, bad, inv, g_nan_equiv);
    total_ok += ok;
    total_bad += bad;
    total_inv += inv;
  }
  say("RESUMEN: %d OK, %d DIFIEREN, %d INVALIDOS%s\n", total_ok, total_bad, total_inv,
      errors ? ", con ERRORES de carga" : "");
  if (total_bad || errors) {
    say("ERROR: la comprobación no pasa\n");
    return 1;
  }
  return 0;
}

#ifndef G2C_CHECK_NO_MAIN
int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "uso: g2c_check <carpeta> <lote> [lote...]\n");
    return 2;
  }
  return g2c_check_main(argv[1], (const char* const*)(argv + 2), NULL);
}
#endif
