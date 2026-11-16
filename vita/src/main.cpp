// Jak Vita — hello world de Fase 0.
// Inicializa vitaGL, limpia la pantalla a un color y escribe un log.
//   X     -> alterna el color de fondo (naranja / azul), prueba que el input y el bucle viven
//   START -> sale limpiamente

#include <cstdint>
#include <vitaGL.h>

#include "log.h"

#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

using jakvita::log;

namespace {

bool file_exists(const char* path) {
  SceIoStat st;
  return sceIoGetstat(path, &st) >= 0;
}

}  // namespace

int main() {
  jakvita::log_init();
  log("Jak Vita hello world (Fase 0) - build %s %s", __DATE__, __TIME__);
  log("CPU %d MHz, GPU %d MHz, bus %d MHz", scePowerGetArmClockFrequency(),
      scePowerGetGpuClockFrequency(), scePowerGetBusClockFrequency());

  // vitaGL compila shaders en tiempo de ejecución con libshacccg.suprx (ShaRKF00D).
  // El clear no lo necesita, pero lo dejamos registrado: el renderer sí lo necesitará.
  const bool shacccg =
      file_exists("ur0:data/libshacccg.suprx") || file_exists("ur0:data/external/libshacccg.suprx");
  log("libshacccg.suprx: %s", shacccg ? "encontrada" : "NO encontrada (usar ShaRKF00D)");

  // Tamaños de pool explícitos: vglInitExtended los calcula con sceAppMgrGetBudgetInfo, que
  // Vita3K solo simula (stub). Además el juego necesitará controlar la memoria a mano.
  // OJO: vglInit* NO devuelve éxito/fallo. Devuelve GL_TRUE solo si tuvo que bajar la
  // resolución porque la pedida supera el máximo de la pantalla (ver vgl.c, res_fallback).
  constexpr int MB = 1024 * 1024;
  const GLboolean res_fallback = vglInitWithCustomSizes(8 * MB,    // pool legacy (modo inmediato)
                                                        960, 544,  // resolución nativa
                                                        32 * MB,   // RAM de usuario
                                                        32 * MB,   // CDRAM (VRAM)
                                                        8 * MB,    // RAM físicamente contigua
                                                        2 * MB,    // common dialog
                                                        SCE_GXM_MULTISAMPLE_NONE);
  if (res_fallback)
    log("AVISO: vitaGL redujo la resolución por debajo de 960x544");
  log("vitaGL inicializado: GL_VENDOR=%s GL_RENDERER=%s", (const char*)glGetString(GL_VENDOR),
      (const char*)glGetString(GL_RENDERER));

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

  bool blue = false;
  uint32_t prev_buttons = 0;
  uint32_t frames = 0, frames_window = 0;
  SceUInt64 window_start = sceKernelGetProcessTimeWide();

  log("Bucle principal: X cambia color, START sale");
  for (;;) {
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const uint32_t pressed = pad.buttons & ~prev_buttons;
    prev_buttons = pad.buttons;

    if (pressed & SCE_CTRL_START) {
      log("START pulsado, saliendo");
      break;
    }
    if (pressed & SCE_CTRL_CROSS) {
      blue = !blue;
      log("Color -> %s", blue ? "azul" : "naranja");
    }

    if (blue)
      glClearColor(0.10f, 0.30f, 0.80f, 1.0f);
    else
      glClearColor(1.00f, 0.55f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    vglSwapBuffers(GL_FALSE);

    frames++;
    frames_window++;
    const SceUInt64 now = sceKernelGetProcessTimeWide();
    if (now - window_start >= 5000000) {  // cada 5 s
      const double secs = (double)(now - window_start) / 1e6;
      log("frames=%u fps=%.1f", (unsigned)frames, frames_window / secs);
      frames_window = 0;
      window_start = now;
    }
  }

  log("Fin. Total frames=%u", (unsigned)frames);
  jakvita::log_shutdown();
  sceKernelExitProcess(0);
  return 0;
}
