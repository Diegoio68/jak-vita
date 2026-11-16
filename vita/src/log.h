#pragma once

// Log de Jak Vita: escribe en stdout y en ux0:data/jakvita/log.txt.
// El fichero se trunca en cada arranque.

namespace jakvita {

void log_init();
void log_shutdown();
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace jakvita
