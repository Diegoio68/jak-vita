/*
 * goal2c (Jak Vita): arnés que ejecuta funciones generadas por goal2c fuera del juego.
 *
 * Compilar junto con un fichero generado, indicando su tabla y su función de enlace:
 *   cc -std=c11 -O2 -I goalc/c_backend/runtime -DG2C_TABLE=g2c_math_funcs \
 *      -DG2C_LINK=g2c_math_link test/goal2c/harness.c out/goal2c/jak1/math.c
 *
 * Entrada (stdin), una llamada por línea:  <nombre-goal> <arg0-hex> <arg1-hex> ...
 *   (cada argumento es el valor de 64 bits del registro GOAL, en hexadecimal)
 * Salida (stdout):
 *   primero "SYM <nombre> <direccion-hex>" para #t y #f,
 *   luego, por cada llamada, "RET <valor-hex>" (o "ERR <motivo>").
 */
#define _POSIX_C_SOURCE 200809L /* strtok_r */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "goal2c_rt.h"

#ifndef G2C_TABLE
#error "define G2C_TABLE (p. ej. -DG2C_TABLE=g2c_math_funcs)"
#endif
#ifndef G2C_LINK
#error "define G2C_LINK (p. ej. -DG2C_LINK=g2c_math_link)"
#endif

extern const g2c_func_entry G2C_TABLE[];
void G2C_LINK(void);

/* memoria GOAL falsa: 1 MB. Los símbolos viven en 0x1000 + 16*i. */
#define MEM_SIZE (1u << 20)
uint8_t* g2c_mem;

static char sym_names[256][64];
static int sym_count;

uint32_t g2c_rt_symbol(const char* name) {
  for (int i = 0; i < sym_count; i++) {
    if (strcmp(sym_names[i], name) == 0) {
      return 0x1000u + 16u * (uint32_t)i;
    }
  }
  if (sym_count >= 256) {
    fprintf(stderr, "demasiados símbolos\n");
    exit(1);
  }
  strncpy(sym_names[sym_count], name, 63);
  return 0x1000u + 16u * (uint32_t)sym_count++;
}

typedef g2c_gpr (*f0)(void);
typedef g2c_gpr (*f1)(g2c_gpr);
typedef g2c_gpr (*f2)(g2c_gpr, g2c_gpr);
typedef g2c_gpr (*f3)(g2c_gpr, g2c_gpr, g2c_gpr);
typedef g2c_gpr (*f4)(g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr);
typedef g2c_gpr (*f5)(g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr);
typedef g2c_gpr (*f6)(g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr);
typedef g2c_gpr (*f7)(g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr);
typedef g2c_gpr (*f8)(g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr, g2c_gpr);

static g2c_gpr call(const g2c_func_entry* e, const g2c_gpr* a) {
  switch (e->nargs) {
    case 0: return ((f0)e->fn)();
    case 1: return ((f1)e->fn)(a[0]);
    case 2: return ((f2)e->fn)(a[0], a[1]);
    case 3: return ((f3)e->fn)(a[0], a[1], a[2]);
    case 4: return ((f4)e->fn)(a[0], a[1], a[2], a[3]);
    case 5: return ((f5)e->fn)(a[0], a[1], a[2], a[3], a[4]);
    case 6: return ((f6)e->fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 7: return ((f7)e->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    default: return ((f8)e->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  }
}

int main(void) {
  g2c_mem = calloc(1, MEM_SIZE);
  if (!g2c_mem) return 1;
  /* #f y #t primero, para que el arnés pueda informar de sus direcciones */
  uint32_t sym_f = g2c_rt_symbol("#f");
  uint32_t sym_t = g2c_rt_symbol("#t");
  G2C_LINK();
  printf("SYM #f %" PRIx32 "\nSYM #t %" PRIx32 "\n", sym_f, sym_t);

  char line[1024];
  while (fgets(line, sizeof(line), stdin)) {
    char* save = NULL;
    char* name = strtok_r(line, " \t\r\n", &save);
    if (!name) continue;
    const g2c_func_entry* e = NULL;
    for (const g2c_func_entry* it = G2C_TABLE; it->name; it++) {
      if (strcmp(it->name, name) == 0) {
        e = it;
        break;
      }
    }
    g2c_gpr args[8] = {0};
    int n = 0;
    char* tok;
    while ((tok = strtok_r(NULL, " \t\r\n", &save)) && n < 8) {
      args[n++] = (g2c_gpr)strtoull(tok, NULL, 16);
    }
    if (!e) {
      printf("ERR funcion-no-encontrada %s\n", name);
    } else if (e->nargs != n) {
      printf("ERR nargs %s espera %d recibe %d\n", name, e->nargs, n);
    } else {
      printf("RET %" PRIx64 "\n", (uint64_t)call(e, args));
    }
    fflush(stdout);
  }
  free(g2c_mem);
  return 0;
}
