#include "log.h"

#include <cstdarg>
#include <cstdio>

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

namespace jakvita {

namespace {
constexpr const char* kLogDir = "ux0:data/jakvita";
constexpr const char* kLogPath = "ux0:data/jakvita/log.txt";
FILE* g_log = nullptr;
}  // namespace

void log_init() {
  sceIoMkdir("ux0:data", 0777);  // ya existe en la consola, pero no cuesta nada
  sceIoMkdir(kLogDir, 0777);
  g_log = std::fopen(kLogPath, "w");
}

void log_shutdown() {
  if (g_log) {
    std::fclose(g_log);
    g_log = nullptr;
  }
}

void log(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  // Marca de tiempo en ms desde el arranque del proceso.
  unsigned ms = (unsigned)(sceKernelGetProcessTimeWide() / 1000);
  std::printf("[%7u] %s\n", ms, buf);
  if (g_log) {
    std::fprintf(g_log, "[%7u] %s\n", ms, buf);
    std::fflush(g_log);  // si crashea, que quede todo escrito
  }
}

}  // namespace jakvita
