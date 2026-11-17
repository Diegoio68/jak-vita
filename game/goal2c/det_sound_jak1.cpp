/*!
 * @file det_sound_jak1.cpp
 * goal2c (Jak Vita): información de sonido determinista para Jak 1 (modo determinista: huella
 * por fotograma y grabación del mando, ver goal2c_jak1.h).
 *
 * El juego lee *sound-iop-info* (posición del stream de audio "strpos", id del stream...), que
 * el IOP escribe en su vblank. Eso depende del tiempo real: el vblank lo dispara el vsync del
 * renderer (con ventana, a la frecuencia de la pantalla; sin ventana, de otra forma) y strpos
 * es lo que la tarjeta de sonido ha reproducido de verdad. Las cinemáticas
 * (ja-play-spooled-anim) avanzan según strpos, así que una grabación hecha con ventana no se
 * reproducía igual sin ventana (desincronización a los ~2 min, al llegar a una cinemática).
 *
 * En modo determinista el IOP no copia su información al EE; la calcula este fichero en el fin
 * de cada fotograma a partir de las órdenes del propio juego (RPC de streams, canal 5):
 *   - orden 0 (reproducir): strpos empieza en 0 en ese fotograma y avanza 1024/60 por fotograma
 *     (strpos va en 1/1024 s), hasta la duración del stream (cabecera VAG en VAGWAD) o una
 *     orden 1 (parar). Sin stream: strpos -1 y str-id 1, como el IOP (StopVAG).
 *   - frame: +1 por fotograma. chinfo (voces sonando): 0 (solo lo usa una vista de depuración).
 * El audio se sigue oyendo igual; solo cambia lo que el juego ve de él.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "goal2c_jak1.h"

#include "common/common_types.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/common/str_rpc_types.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/overlord/common/fake_iso.h"
#include "game/overlord/common/srpc.h"
#include "game/overlord/jak1/iso.h"

namespace jak1 {
extern u32 gFreeMem;
extern u32 gMemFreeAtStart;
}  // namespace jak1

namespace goal2c_runtime {
namespace {

struct DetStream {
  bool playing = false;
  u32 id = 1;
  u64 start_frame = 0;
  s64 duration = 0;  // en 1/1024 s; <= 0: sin fin conocido (solo para con la orden de parar)
};
DetStream g_stream;
u64 g_det_frame = 0;

//! Duración (1/1024 s) del stream VAG de esa entrada del directorio, leyendo su cabecera en
//! VAGWAD.<idioma>. 0 si no se puede leer.
s64 vag_duration(jak1::VagDirEntry* vag) {
  char wad[16];
  memcpy(wad, "VAGWAD  ", 8);
  strcpy(&wad[8], gLanguage);
  FileRecord* fr = jak1::isofs ? jak1::isofs->find_in(wad) : nullptr;
  if (!fr) {
    return 0;
  }
  const char* path = get_file_path(fr);
  FILE* f = path ? fopen(path, "rb") : nullptr;
  if (!f) {
    return 0;
  }
  u8 h[20];
  bool ok = fseek(f, (long)vag->offset * 2048, SEEK_SET) == 0 && fread(h, 1, 20, f) == 20 &&
            memcmp(h, "VAGp", 4) == 0;
  fclose(f);
  if (!ok) {
    return 0;
  }
  u32 size = (u32)h[12] << 24 | (u32)h[13] << 16 | (u32)h[14] << 8 | h[15];
  u32 rate = (u32)h[16] << 24 | (u32)h[17] << 16 | (u32)h[18] << 8 | h[19];
  if (rate == 0) {
    return 0;
  }
  // misma fórmula que el reloj real del IOP (iso.cpp, UpdatePlayPos)
  return (s64)(4ULL * 0x1C00ULL * (size / 16) / rate);
}

//! Nombre de la orden -> entrada del directorio VAG (como RPC_PLAY en stream.cpp)
jak1::VagDirEntry* find_vag(const char* cmd_name) {
  char namebuf[16];
  if (cmd_name[0] == '$') {
    const char* name_part = &cmd_name[1];
    size_t name_len = strnlen(name_part, 47);
    if (name_len < 9) {
      memset(namebuf, ' ', 8);
      memcpy(namebuf, name_part, name_len);
    } else {
      memcpy(namebuf, name_part, 8);
    }
    for (int i = 0; i < 8; i++) {
      if (namebuf[i] >= 0x61 && namebuf[i] < 0x7b) {
        namebuf[i] -= 0x20;
      }
    }
  } else {
    file_util::ISONameFromAnimationName(namebuf, cmd_name);
  }
  namebuf[8] = 0;
  return jak1::FindVAGFile(namebuf);
}

}  // namespace

//! diagnóstico: GOAL2C_SONIDO_REAL=1 deja el sonido como antes (el del IOP, en tiempo real)
bool det_sound_on() {
  static const bool on = deterministic_mode() && !std::getenv("GOAL2C_SONIDO_REAL");
  return on;
}

void det_play_rpc(const void* send, int size) {
  if (!det_sound_on()) {
    return;
  }
  constexpr int kMsgSize = 0x40;  // PLAY_MSG_SIZE (stream.cpp)
  static_assert(sizeof(RPC_Play_Cmd_Jak1) == kMsgSize, "RPC_Play_Cmd_Jak1");
  for (int off = 0; off + kMsgSize <= size; off += kMsgSize) {
    RPC_Play_Cmd_Jak1 cmd;
    memcpy(&cmd, (const u8*)send + off, sizeof(cmd));
    cmd.name[47] = 0;
    if (cmd.result == 0) {  // reproducir
      auto* vag = find_vag(cmd.name);
      g_stream.playing = true;
      g_stream.id = cmd.address;
      g_stream.start_frame = g_det_frame;
      g_stream.duration = vag ? vag_duration(vag) : 0;
      lg::debug("[sonido det.] stream {} (id {}) {} ms", cmd.name, cmd.address,
                g_stream.duration * 1000 / 1024);
    } else if (cmd.result == 1) {  // parar
      g_stream.playing = false;
      g_stream.id = 1;
    }
  }
}

void det_sound_frame() {
  if (!det_sound_on()) {
    return;
  }
  g_det_frame++;
  auto sym = jak1::find_symbol_from_c("*sound-iop-info*");
  u32 info = sym.offset ? *Ptr<u32>(sym.offset).c() : 0;
  if (!info || info == (u32)s7.offset) {
    return;
  }
  s32 strpos = -1;
  if (g_stream.playing) {
    s64 pos = (s64)((g_det_frame - g_stream.start_frame) * 1024 / 60);
    if (g_stream.duration > 0 && pos >= g_stream.duration) {
      g_stream.playing = false;  // fin natural del stream
      g_stream.id = 1;
    } else {
      strpos = (s32)pos;
    }
  }
  // sound-iop-info (basic): frame +16, strpos +20, str-id +24, freemem +28, chinfo +32 (48),
  // freemem2 +80 (offsets de campo; la dirección del objeto es la del tipo + 4)
  u8* p = Ptr<u8>(info - 4).c();
  u32 frame = (u32)g_det_frame;
  memcpy(p + 16, &frame, 4);
  memcpy(p + 20, &strpos, 4);
  memcpy(p + 24, &g_stream.id, 4);
  memcpy(p + 28, &jak1::gFreeMem, 4);
  memset(p + 32, 0, 48);
  memcpy(p + 80, &jak1::gMemFreeAtStart, 4);
}

void det_sound_reset() {
  g_stream = DetStream{};
  g_det_frame = 0;
}

}  // namespace goal2c_runtime
