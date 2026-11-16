#pragma once

// Log de Jak Vita: escribe en stdout y en un fichero de ux0:data/jakvita/
// (por defecto log.txt). El fichero se trunca en cada arranque.

namespace jakvita {

void log_init(const char* path = "ux0:data/jakvita/log.txt");
void log_shutdown();
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace jakvita
