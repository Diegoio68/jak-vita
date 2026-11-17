/*!
 * @file main.c
 * goal2c (Jak Vita): app de comprobación JAKV00003. Reproduce en ARM los casos grabados en el
 * PC (app0:check/<lote>.rec + .img.z) con el C de goal2c y los compara con goalc (x86).
 * Toda la lógica está en goalc/c_backend/check/g2c_check.c (la misma que en PC).
 * Log: ux0:data/jakvita/check.txt (y stdout). Contiene "ERROR" si algún caso difiere.
 */
#include <stdio.h>

#include "check_lots.h" /* G2C_CHECK_LOTS: lista de lotes, la genera CMake */

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

int g2c_check_main(const char* dir, const char* const* lots, FILE* out);

/* la imagen de la memoria GOAL ocupa decenas de MB; la pila de C acoge las variables de pila
   de GOAL (G2C_ADDR solo acepta la pila del hilo principal fuera de la imagen) */
int _newlib_heap_size_user = 256 * 1024 * 1024;
int sceUserMainThreadStackSize = 4 * 1024 * 1024;

int main(void) {
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir("ux0:data/jakvita", 0777);
  FILE* out = fopen("ux0:data/jakvita/check.txt", "w");
  /* G2C_CHECK_LOTS ya termina en coma: "lote_a", "lote_b", */
  static const char* const lots[] = {G2C_CHECK_LOTS 0};
  if (out) {
    fprintf(out, "goal2c check (ARM): %d lote(s)\n", (int)(sizeof(lots) / sizeof(lots[0])) - 1);
  }
  g2c_check_main("app0:check", lots, out);
  if (out) {
    fprintf(out, "FIN\n");
    fclose(out);
  }
  /* seguir vivo hasta que el script cierre el emulador (emu_test.sh comprueba que la app vive) */
  for (;;) {
    sceKernelDelayThread(1000 * 1000);
  }
  return 0;
}
