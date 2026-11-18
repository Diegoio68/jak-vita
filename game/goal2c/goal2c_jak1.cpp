/*!
 * @file goal2c_jak1.cpp
 * goal2c (Jak Vita): modo mixto del runtime de PC para Jak 1. Ver goal2c_jak1.h.
 *
 * Funcionamiento:
 * - x86 -> C: cada función traducida se envuelve en un objeto función GOAL cuyo código
 *   (x86-64) guarda r13 (el registro pp de GOAL, el proceso actual) en g_pp y salta al
 *   trampolín del kernel _arg_call_systemv (o _stack_call_systemv con más de 6 argumentos),
 *   que preserva los registros que GOAL espera y llama a la función C.
 * - C -> x86: g2c_host_call usa _call_goal8_asm_systemv, que fija s7, r15 (base de memoria)
 *   y r13 = g_pp, y llama al objeto función GOAL (sea x86 o un envoltorio de C).
 * - Argumentos/retorno de 128 bits (goalc x86: xmm1..xmm8 y xmm0): x86 -> C pasa por
 *   g2c_q_call_systemv, que vuelca los 8 GPR y los 8 xmm de argumentos en arrays y llama al
 *   envoltorio __q de la función; C -> x86 usa g2c_call_goal_q, que carga los xmm.
 * - Datos estáticos: el C pide (segmento, posición); la base del segmento sale de la
 *   cabecera del objeto recién enlazado. Igual para la dirección de las lambdas (mov-fa).
 * - Funciones sin símbolo ni método (lambdas: estados, behaviors anónimos...): se parchea su
 *   código x86 in situ con un salto al envoltorio de C (si mide al menos 12 bytes).
 * - suspend: thread-suspend/resume nativos (copia de la pila a un búfer del host, sin el límite
 *   de 256 bytes del área de respaldo); ver install_native_suspend. Con GOAL2C_SUSPEND_X86 las
 *   que escriben pp (G2C_FN_WRITES_PP: suspend, with-proc) se quedan en x86.
 */
#include "goal2c_jak1.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"
#include "common/log/log.h"
#include "common/symbols.h"

#include "game/kernel/common/codegen.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/common/memory_layout.h"
#include "game/kernel/jak1/klisten.h"
#include "game/kernel/jak1/kscheme.h"
#include "goalc/c_backend/runtime/goal2c_rt.h"

#if defined(__linux__) && defined(__x86_64__)
#define GOAL2C_MIXED_SUPPORTED 1
#include <csignal>
#include <cstdio>
#include <dlfcn.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

extern "C" {
void _arg_call_systemv() asm("_arg_call_systemv");
void _stack_call_systemv() asm("_stack_call_systemv");
u64 _call_goal8_asm_systemv(void* func, u64* arg_array, u64 zero, u64 pp, u64 st, void* off) asm(
    "_call_goal8_asm_systemv");
void g2c_q_call_systemv() asm("g2c_q_call_systemv");
u64 g2c_call_goal_q(void* func, void* block, u64 pp, u64 st, void* off) asm("g2c_call_goal_q");
}

// Trampolines para argumentos/retorno de 128 bits. Convenio de goalc x86: GPR en rdi, rsi,
// rdx, rcx, r8, r9, r10, r11; 128 bits en xmm1..xmm8; retorno en rax / xmm0. GOAL espera que
// se conserven rbx, rbp, r10, r11, r12 y xmm8..xmm15 (SysV no conserva r10, r11 ni los xmm).
//
// g2c_q_call_systemv (x86 -> C): el objeto función de entrada deja en la pila la dirección
// del envoltorio C  g2c_gpr f(const u64* g8, const g2c_q* q8, g2c_q* qret)  y salta aquí.
// Marco (rsp alineado a 16 tras el sub): [0,16) qret, [16,144) q8, [144,208) g8,
// [208,336) xmm8..xmm15 guardados, [336,344) relleno.
//
// g2c_call_goal_q (C -> x86): rdi = función, rsi = bloque {u64 g8[8]; g2c_q q8[8]; g2c_q
// qret} alineado a 16, rdx = pp (r13), rcx = tabla de símbolos (r14), r8 = base (r15).
asm(R"(
  .text
  .intel_syntax noprefix
  .globl g2c_q_call_systemv
g2c_q_call_systemv:
  pop rax
  sub rsp, 344
  movaps [rsp + 208], xmm8
  movaps [rsp + 224], xmm9
  movaps [rsp + 240], xmm10
  movaps [rsp + 256], xmm11
  movaps [rsp + 272], xmm12
  movaps [rsp + 288], xmm13
  movaps [rsp + 304], xmm14
  movaps [rsp + 320], xmm15
  mov [rsp + 144], rdi
  mov [rsp + 152], rsi
  mov [rsp + 160], rdx
  mov [rsp + 168], rcx
  mov [rsp + 176], r8
  mov [rsp + 184], r9
  mov [rsp + 192], r10
  mov [rsp + 200], r11
  movaps [rsp + 16], xmm1
  movaps [rsp + 32], xmm2
  movaps [rsp + 48], xmm3
  movaps [rsp + 64], xmm4
  movaps [rsp + 80], xmm5
  movaps [rsp + 96], xmm6
  movaps [rsp + 112], xmm7
  movaps [rsp + 128], xmm8
  lea rdi, [rsp + 144]
  lea rsi, [rsp + 16]
  mov rdx, rsp
  call rax
  movaps xmm0, [rsp]
  mov r10, [rsp + 192]
  mov r11, [rsp + 200]
  movaps xmm8, [rsp + 208]
  movaps xmm9, [rsp + 224]
  movaps xmm10, [rsp + 240]
  movaps xmm11, [rsp + 256]
  movaps xmm12, [rsp + 272]
  movaps xmm13, [rsp + 288]
  movaps xmm14, [rsp + 304]
  movaps xmm15, [rsp + 320]
  add rsp, 344
  mov r13, [rip + g2c_pp_cell]
  ret

  .globl g2c_call_goal_q
g2c_call_goal_q:
  push r13
  push r14
  push r15
  push rbx
  mov rbx, rsi
  mov r13, rdx
  mov r14, rcx
  mov r15, r8
  mov rax, rdi
  movaps xmm1, [rbx + 64]
  movaps xmm2, [rbx + 80]
  movaps xmm3, [rbx + 96]
  movaps xmm4, [rbx + 112]
  movaps xmm5, [rbx + 128]
  movaps xmm6, [rbx + 144]
  movaps xmm7, [rbx + 160]
  movaps xmm8, [rbx + 176]
  mov rdi, [rbx]
  mov rsi, [rbx + 8]
  mov rdx, [rbx + 16]
  mov rcx, [rbx + 24]
  mov r8, [rbx + 32]
  mov r9, [rbx + 40]
  mov r10, [rbx + 48]
  mov r11, [rbx + 56]
  call [rip + g2c_goal_call_pad]
  movaps [rbx + 192], xmm0
  pop rbx
  pop r15
  pop r14
  pop r13
  ret
  .att_syntax prefix
)");

// Llamada C -> x86 con 8 GPR (como _call_goal8_asm_systemv) que además deja en *pp_out el r13
// FINAL: tras un suspend + resume, pp es el que puso el kernel (el proceso, quizá reubicado),
// no el que había al llamar.
//   rdi = función, rsi = u64 args[8], rdx = pp, rcx = tabla de símbolos, r8 = base, r9 = pp_out
extern "C" u64 g2c_call_goal8(void* func, const u64* args, u64 pp, u64 st, void* off, u64* pp_out)
    asm("g2c_call_goal8");

// pp actual visto desde C (g_pp); el asm lo lee para recargar r13 al volver de C a x86
extern "C" {
u64 g2c_pp_cell asm("g2c_pp_cell") = 0;
// Rampa C -> GOAL en la memoria GOAL ("call rax; ret"; ver install_call_pad): las llamadas de C a
// GOAL pasan por ella para que la función GOAL vea una dirección de vuelta DENTRO de la memoria
// GOAL. Código GOAL que la guarda como dirección GOAL de 32 bits (el new de catch-frame: catch y
// throw de run-function-in-process) fallaba con la dirección del host.
u64 g2c_goal_call_pad asm("g2c_goal_call_pad") = 0;
}
// x86 -> C como _arg_call_systemv / _stack_call_systemv del kernel (mismo marco), pero al volver
// recargan r13 de g2c_pp_cell (ver g_pp).
extern "C" void g2c_arg_call() asm("g2c_arg_call");
extern "C" void g2c_stack_call() asm("g2c_stack_call");

// Suspend/resume nativos (ver "Suspend nativo" más abajo). Se entran desde código GOAL x86
// (objeto función con movabs rax + jmp rax), con r14 = tabla de símbolos y r15 = base.
// Campos (dirección GOAL t de un cpu-thread, basic: campo de offset o en t - 4 + o):
//   process t+4, pc t+20, sp t+24, stack-top t+28, rreg[7] t+36, freg[8] t+92, stack t+124.
extern "C" void g2c_native_suspend() asm("g2c_native_suspend");
extern "C" void g2c_native_resume() asm("g2c_native_resume");
// helpers en C++ (devuelven la rsp real del kernel / no devuelven nada)
extern "C" u64 g2c_suspend_save(u64 thread, u64 rsp) asm("g2c_suspend_save");
extern "C" u64 g2c_resume_restore(u64 thread, u64 kernel_rsp) asm("g2c_resume_restore");

asm(R"(
  .text
  .intel_syntax noprefix
  .globl g2c_arg_call
g2c_arg_call:
  pop rax
  push r10
  push r11
  sub rsp, 136
  movaps [rsp], xmm8
  movaps [rsp + 16], xmm9
  movaps [rsp + 32], xmm10
  movaps [rsp + 48], xmm11
  movaps [rsp + 64], xmm12
  movaps [rsp + 80], xmm13
  movaps [rsp + 96], xmm14
  movaps [rsp + 112], xmm15
  call rax
  movaps xmm8, [rsp]
  movaps xmm9, [rsp + 16]
  movaps xmm10, [rsp + 32]
  movaps xmm11, [rsp + 48]
  movaps xmm12, [rsp + 64]
  movaps xmm13, [rsp + 80]
  movaps xmm14, [rsp + 96]
  movaps xmm15, [rsp + 112]
  add rsp, 136
  pop r11
  pop r10
  mov r13, [rip + g2c_pp_cell]
  ret

  .globl g2c_stack_call
g2c_stack_call:
  pop rax
  sub rsp, 136
  movaps [rsp], xmm8
  movaps [rsp + 16], xmm9
  movaps [rsp + 32], xmm10
  movaps [rsp + 48], xmm11
  movaps [rsp + 64], xmm12
  movaps [rsp + 80], xmm13
  movaps [rsp + 96], xmm14
  movaps [rsp + 112], xmm15
  push r11
  push r10
  push r9
  push r8
  push rcx
  push rdx
  push rsi
  push rdi
  mov rdi, rsp
  call rax
  pop rdi
  pop rsi
  pop rdx
  pop rcx
  pop r8
  pop r9
  pop r10
  pop r11
  movaps xmm8, [rsp]
  movaps xmm9, [rsp + 16]
  movaps xmm10, [rsp + 32]
  movaps xmm11, [rsp + 48]
  movaps xmm12, [rsp + 64]
  movaps xmm13, [rsp + 80]
  movaps xmm14, [rsp + 96]
  movaps xmm15, [rsp + 112]
  add rsp, 136
  mov r13, [rip + g2c_pp_cell]
  ret

  .globl g2c_call_goal8
g2c_call_goal8:
  push r13
  push r14
  push r15
  push r9
  mov r13, rdx
  mov r14, rcx
  mov r15, r8
  mov rax, rdi
  mov rdi, [rsi + 0]
  mov rdx, [rsi + 16]
  mov rcx, [rsi + 24]
  mov r8, [rsi + 32]
  mov r9, [rsi + 40]
  mov r10, [rsi + 48]
  mov r11, [rsi + 56]
  mov rsi, [rsi + 8]
  call [rip + g2c_goal_call_pad]
  pop r9
  mov [r9], r13
  pop r15
  pop r14
  pop r13
  ret

  # thread-suspend nativo: r13 = hilo (dirección GOAL). Igual que el de gkernel.gc, pero la
  # copia de la pila va a un búfer del host (g2c_suspend_save) sin límite de tamaño.
  .globl g2c_native_suspend
g2c_native_suspend:
  # la dirección de retorno se queda en la pila (se guarda con ella): si se suspende desde C es
  # una dirección del host, que no cabe en pc (32 bits). pc solo se usa en el arranque.
  mov rax, [rsp]
  sub rax, r15
  mov [r15 + r13 + 20], eax
  mov rax, rsp
  sub rax, r15
  mov [r15 + r13 + 24], eax
  mov [r15 + r13 + 36], rbx
  mov [r15 + r13 + 44], rbp
  mov [r15 + r13 + 52], r10
  mov [r15 + r13 + 60], r11
  mov [r15 + r13 + 68], r12
  movss [r15 + r13 + 92], xmm8
  movss [r15 + r13 + 96], xmm9
  movss [r15 + r13 + 100], xmm10
  movss [r15 + r13 + 104], xmm11
  movss [r15 + r13 + 108], xmm12
  movss [r15 + r13 + 112], xmm13
  movss [r15 + r13 + 116], xmm14
  movss [r15 + r13 + 120], xmm15
  # la copia se hace en C++, con la pila por debajo de sp (la parte usada ya está guardada)
  mov rdi, r13
  mov rsi, rsp
  and rsp, -16
  call g2c_suspend_save
  # rax = rsp real del kernel (*kernel-sp* + base)
  xor r13d, r13d
  mov rsp, rax
  pop r12
  pop r11
  pop r10
  pop rbp
  pop rbx
  ret

  # thread-resume nativo: rdi = hilo. Lo llama el kernel (resume-hook).
  .globl g2c_native_resume
g2c_native_resume:
  push rbx
  push rbp
  push r10
  push r11
  push r12
  mov r13, rdi
  mov rbx, rsp
  mov rsi, rsp
  and rsp, -16
  call g2c_resume_restore
  # rax = 1 si se restauró una pila guardada (se vuelve con ret), 0 si el hilo arranca (pc)
  mov rdx, rax
  mov eax, [r15 + r13 + 24]
  add rax, r15
  mov rsp, rax
  mov rbx, [r15 + r13 + 36]
  mov rbp, [r15 + r13 + 44]
  mov r10, [r15 + r13 + 52]
  mov r11, [r15 + r13 + 60]
  mov r12, [r15 + r13 + 68]
  movss xmm8, [r15 + r13 + 92]
  movss xmm9, [r15 + r13 + 96]
  movss xmm10, [r15 + r13 + 100]
  movss xmm11, [r15 + r13 + 104]
  movss xmm12, [r15 + r13 + 108]
  movss xmm13, [r15 + r13 + 112]
  movss xmm14, [r15 + r13 + 116]
  movss xmm15, [r15 + r13 + 120]
  # set-to-run-bootstrap: rreg 5 y 6 en r8 y r9 (como el resume de gkernel.gc)
  mov r8, [r15 + r13 + 76]
  mov r9, [r15 + r13 + 84]
  mov eax, [r15 + r13 + 20]
  add rax, r15
  mov r13d, [r15 + r13 + 4]
  test rdx, rdx
  jz 1f
  ret
1:
  jmp rax
  .att_syntax prefix
)");
#endif

namespace jak1 {
u64 method_set(u32 type_, u32 method_id, u32 method);
}

namespace goal2c_runtime {

#ifdef GOAL2C_MIXED_SUPPORTED
namespace {

using namespace jak1_symbols;

std::string g_loaded_path;  // librería cargada ahora mismo ("" = ninguna)
std::map<std::string, const g2c_file_info*> g_files;
// objeto -> {offset, size} de sus 3 segmentos (todos los objetos v3 enlazados, para el grabador)
std::map<std::string, std::array<u32, 6>> g_obj_segments;
std::map<std::string, int> g_replaced;  // objeto -> funciones sustituidas en su último enlace
ObjectFileHeader* g_cur_ofh = nullptr;
std::string g_cur_obj;

// r13 (pp) de la última entrada x86 -> C. host_call lo actualiza con el r13 final de cada llamada
// C -> x86, y los trampolines x86 -> C (g2c_arg_call...) recargan r13 de aquí al volver: si el C
// suspendió y el proceso se reubicó, el llamador x86 debe ver el pp nuevo (no el que tenía al
// entrar, que el C conserva por ser r13 callee-saved en SysV).
u64& g_pp = g2c_pp_cell;

// Zona de los objetos función de entrada a C: fuera de los heaps de GOAL, en el hueco sin usar
// entre el final del heap global y el heap de depuración. Así el heap del juego queda idéntico
// al de una ejecución todo x86 (las direcciones de los objetos del juego no cambian y se pueden
// comparar estructuras enteras, p. ej. la cadena DMA de cada fotograma). Huecos de 64 bytes:
// etiqueta de tipo en +16 y código en +20 (44 bytes; el código de entrada mide 36). El objeto
// función es un basic: su dirección debe ser 4 (mod 8), como la de los de goalc. Con el código en
// +16 (0 mod 8) valid? lo rechazaba ("misaligned") y birth! no hacía nacer los actores cuyo
// init-from-entity! estaba en C (fallo de casi todo common-obs, 2026-11-17).
constexpr u32 kEntryZoneStart = 0x4e00000;
constexpr u32 kEntryZoneEnd = DEBUG_HEAP_START;
constexpr u32 kEntrySlot = 0x40;
static_assert(kEntryZoneStart >= GLOBAL_HEAP_END, "la zona de entradas pisa el heap global");
// el primer hueco de la zona es un símbolo ficticio (valor #f) para los símbolos que no existen
constexpr u32 kDummySymbol = kEntryZoneStart + 0x10;
u32 g_entry_next = kEntryZoneStart + kEntrySlot;
u64 g_fingerprint_frame = 0;  // fotogramas desde el arranque del runtime (huella)
u64 g_vclock_in_frame = 0;    // reloj virtual: ticks dentro del fotograma actual
s64 g_script_start = -1;      // recorrido guionizado: fotograma en que apareció *target*
u32 g_salto_string = 0;       // GOAL2C_SALTO: nombre del punto de continuación (string GOAL)
bool g_salto_hecho = false;
// salto de nivel: de GOAL2C_SALTO / GOAL2C_SALTO_FOTOGRAMA o de la línea "# salto <punto> <n>"
// de la grabación (ver load_script); vacío = sin salto
std::string g_salto_name;
u64 g_salto_at = 600;
bool g_salto_env_read = false;
// progreso que se aplica tras el salto (GOAL2C_PROGRESO=tarea,tarea / GOAL2C_CELULAS=n, o las
// líneas "# progreso ..." y "# celulas n" de la grabación): tareas con todas sus etapas cerradas
std::vector<int> g_progreso_tareas;
int g_progreso_celulas = -1;

void salto_config_from_env() {
  if (g_salto_env_read) {
    return;
  }
  g_salto_env_read = true;
  if (const char* n = std::getenv("GOAL2C_SALTO"); n && *n) {
    g_salto_name = n;
    if (const char* f = std::getenv("GOAL2C_SALTO_FOTOGRAMA"); f && *f) {
      g_salto_at = std::strtoull(f, nullptr, 10);
    }
    if (const char* p = std::getenv("GOAL2C_PROGRESO"); p && *p) {
      for (const char* c = p; *c;) {
        g_progreso_tareas.push_back(std::atoi(c));
        while (*c && *c != ',') c++;
        if (*c == ',') c++;
      }
    }
    if (const char* c = std::getenv("GOAL2C_CELULAS"); c && *c) {
      g_progreso_celulas = std::atoi(c);
    }
  }
}

u32 host_symbol(const char* name) {
  // Casi siempre el símbolo ya existe (el objeto x86 lo internó al enlazarse). Si no, crearlo
  // reserva su string en el heap global y el heap deja de ser igual al de una ejecución x86.
  auto sym = jak1::find_symbol_from_c(name);
  if (sym.offset) {
    return sym.offset;
  }
  // Solo pasa con código del segmento de depuración que no se ha cargado (sin -debug): sus
  // símbolos no existen porque el x86 de ese segmento nunca se enlazó. Esas funciones no se
  // instalan; se les da un símbolo ficticio (valor #f) en vez de crear uno, que reservaría su
  // string en el heap global y lo haría distinto al de una ejecución x86.
  lg::debug("[goal2c] símbolo {} inexistente: se usa el ficticio", name);
  return kDummySymbol;
}

u32 host_static(const char* obj, int seg, int off) {
  if (!g_cur_ofh || g_cur_obj != obj || seg < 0 || seg >= N_SEG) {
    lg::error("[goal2c] estático pedido fuera del enlace de su objeto: {} seg {} +{}", obj, seg,
              off);
    return 0;
  }
  return g_cur_ofh->code_infos[seg].offset + (u32)off;
}

// Con el suspend nativo, el aviso de KERNEL_DEBUG del macro suspend (gkernel-h.gc: "ERROR:
// suspend called without enough stack", si la pila usada no cabe en el área de respaldo de
// 128/256 bytes) no aplica: la pila se guarda fuera, sin límite. Se omite esa llamada a format.
u32 g_format_fn = 0;
bool g_suspend_native_on = false;
constexpr char kSuspendStackMsg[] = "ERROR: suspend called without enough stack";

u64 host_call(u32 fn, int nargs, const u64* args) {
  if (fn == g_format_fn && g_suspend_native_on && nargs >= 2 && (u32)args[1]) {
    const char* str = Ptr<String>((u32)args[1])->data();
    if (strncmp(str, kSuspendStackMsg, sizeof(kSuspendStackMsg) - 1) == 0) {
      return 0;
    }
  }
  u64 a8[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < nargs && i < 8; i++) {
    a8[i] = args[i];
  }
  // g_pp queda con el r13 final: tras un suspend + resume es el que fijó el kernel (el proceso,
  // quizá reubicado); en una llamada normal, el mismo que se pasó.
  return g2c_call_goal8(g_ee_main_mem + fn, a8, g_pp, s7.offset, g_ee_main_mem, &g_pp);
}

struct alignas(16) QCallBlock {
  u64 g[8];
  g2c_q q[8];
  g2c_q ret;
};
static_assert(offsetof(QCallBlock, q) == 64 && offsetof(QCallBlock, ret) == 192,
              "QCallBlock: posiciones que usa g2c_call_goal_q");

u64 host_call_q(u32 fn, const u64* g8, const void* q8, void* qret) {
  QCallBlock b;
  memcpy(b.g, g8, sizeof(b.g));
  if (q8) {
    memcpy(b.q, q8, sizeof(b.q));
  } else {
    memset(b.q, 0, sizeof(b.q));
  }
  memset(&b.ret, 0, sizeof(b.ret));
  u64 saved_pp = g_pp;
  u64 ret = g2c_call_goal_q(g_ee_main_mem + fn, &b, saved_pp, s7.offset, g_ee_main_mem);
  g_pp = saved_pp;
  memcpy(qret, &b.ret, sizeof(b.ret));
  return ret;
}

void emit_movabs_rax(u8* code, int* off, u64 imm) {
  code[(*off)++] = 0x48;  // movabs rax, imm64
  code[(*off)++] = 0xb8;
  memcpy(code + *off, &imm, 8);
  *off += 8;
}

enum class Entry { ARGS, STACK, Q };

//! Objeto función GOAL que entra en la función C fn (ver comentario del fichero).
u32 make_c_entry(void* fn, Entry kind) {
  if (g_entry_next + kEntrySlot > kEntryZoneEnd) {
    lg::die("[goal2c] zona de entradas a C llena (0x{:x}..0x{:x})", kEntryZoneStart, kEntryZoneEnd);
  }
  u32 slot = g_entry_next;
  g_entry_next += kEntrySlot;
  memset(Ptr<u8>(slot).c(), 0, kEntrySlot);
  *Ptr<u32>(slot + 16).c() = *(s7 + FIX_SYM_FUNCTION_TYPE);
  auto mem = Ptr<u8>(slot + 20);
  u8* code = mem.c();
  int off = 0;
  // movabs rax, &g_pp ; mov [rax], r13
  emit_movabs_rax(code, &off, (u64)&g_pp);
  code[off++] = 0x4c;
  code[off++] = 0x89;
  code[off++] = 0x28;
  // movabs rax, fn ; push rax  (el trampolín lo saca con pop rax)
  emit_movabs_rax(code, &off, (u64)fn);
  code[off++] = 0x50;
  // movabs rax, trampolín ; jmp rax
  emit_movabs_rax(code, &off,
                  kind == Entry::Q       ? (u64)&g2c_q_call_systemv
                  : kind == Entry::STACK ? (u64)&g2c_stack_call
                                         : (u64)&g2c_arg_call);
  code[off++] = 0xff;
  code[off++] = 0xe0;
  ASSERT(20 + off <= (int)kEntrySlot);
  flush_icache_goal(mem.offset, off);
  return mem.offset;
}

//! Diagnóstico de fallos en modo mixto: imprime dónde ha fallado (función y desplazamiento
//! dentro de la librería de goal2c, para addr2line) y la alineación de la pila.
struct sigaction g_prev_segv;
void segv_handler(int sig, siginfo_t* info, void* vctx) {
  auto* uc = (ucontext_t*)vctx;
  void* rip = (void*)uc->uc_mcontext.gregs[REG_RIP];
  u64 rsp = (u64)uc->uc_mcontext.gregs[REG_RSP];
  Dl_info dli{};
  char buf[512];
  int n = 0;
  if (dladdr(rip, &dli) && dli.dli_fname) {
    n = snprintf(buf, sizeof(buf),
                 "[goal2c] SIGSEGV en %p (%s + 0x%lx, %s), dirección %p, rsp %% 16 = %lu\n", rip,
                 dli.dli_fname, (unsigned long)((u8*)rip - (u8*)dli.dli_fbase),
                 dli.dli_sname ? dli.dli_sname : "?", info->si_addr, (unsigned long)(rsp % 16));
  } else {
    n = snprintf(buf, sizeof(buf), "[goal2c] SIGSEGV en %p (fuera de librerías), dirección %p\n",
                 rip, info->si_addr);
  }
  if (n > 0) {
    (void)!write(2, buf, (size_t)n);
  }
  // pila de llamadas por rbp (fiable con la librería compilada con --debug, -O0)
  u64* fp = (u64*)uc->uc_mcontext.gregs[REG_RBP];
  // (en código GOAL rbp es un registro más: solo se sigue si apunta a la pila)
  for (int depth = 0; depth < 8 && (u64)fp > rsp && (u64)fp < rsp + 0x100000; depth++) {
    void* ret = (void*)fp[1];
    Dl_info d{};
    if (!dladdr(ret, &d) || !d.dli_fname || !strstr(d.dli_fname, "goal2c")) {
      break;  // solo nos interesan los marcos dentro de la librería de goal2c
    }
    n = snprintf(buf, sizeof(buf), "[goal2c]   llamado desde %s + 0x%lx\n", d.dli_fname,
                 (unsigned long)((u8*)ret - (u8*)d.dli_fbase));
    if (n > 0) {
      (void)!write(2, buf, (size_t)n);
    }
    fp = (u64*)fp[0];
  }
  // Señal enviada a mano (kill/tgkill: SI_USER/SI_TKILL), para ver dónde está un proceso
  // colgado: lista los valores de la pila que parecen direcciones de retorno (gk, librerías o
  // código GOAL). Heurístico (sin marcos fiables en -O2), pero suele bastar.
  {  // en cualquier fallo (también señal enviada a mano)
    u64 mem = (u64)g_ee_main_mem;
    u64* sp = (u64*)rsp;
    int shown = 0;
    n = snprintf(buf, sizeof(buf), "[goal2c]   r13 (pp) 0x%llx, rdi 0x%llx\n",
                 (unsigned long long)uc->uc_mcontext.gregs[REG_R13],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_RDI]);
    (void)!write(2, buf, (size_t)n);
    for (int i = 0; i < 4096 && shown < 60; i++) {
      u64 v = sp[i];
      Dl_info d{};
      if (v >= mem + 0x100000 && v < mem + 0x8000000) {
        n = snprintf(buf, sizeof(buf), "[goal2c]   pila+0x%x: GOAL 0x%llx\n", i * 8,
                     (unsigned long long)(v - mem));
      } else if (v > 0x10000 && dladdr((void*)v, &d) && d.dli_fname) {
        n = snprintf(buf, sizeof(buf), "[goal2c]   pila+0x%x: %s + 0x%lx (%s)\n", i * 8,
                     d.dli_fname, (unsigned long)((u8*)v - (u8*)d.dli_fbase),
                     d.dli_sname ? d.dli_sname : "?");
      } else {
        continue;
      }
      shown++;
      (void)!write(2, buf, (size_t)n);
    }
  }
  // devolver el control al manejador anterior (o al de por defecto)
  sigaction(SIGSEGV, &g_prev_segv, nullptr);
  raise(sig);
}

// ---- GOAL2C_WATCH=<dirección GOAL hex>[,<bytes>]: quién escribe en esa zona (diagnóstico) ----
// Protege la página contra escritura; cada escritura a la zona se anota (rip como dirección GOAL
// o librería + desplazamiento, y el valor antes/después) y la instrucción se ejecuta paso a paso.
u32 g_watch_addr = 0, g_watch_len = 0;
u8* g_watch_page = nullptr;
u32 g_watch_before = 0;
bool g_watch_hit_in_range = false;
int g_watch_count = 0;
struct sigaction g_watch_prev_segv;

void watch_describe(void* rip, char* buf, size_t n) {
  u8* r = (u8*)rip;
  Dl_info dli{};
  if (r >= g_ee_main_mem && r < g_ee_main_mem + EE_MAIN_MEM_SIZE) {
    snprintf(buf, n, "GOAL 0x%lx", (unsigned long)(r - g_ee_main_mem));
  } else if (dladdr(rip, &dli) && dli.dli_fname) {
    snprintf(buf, n, "%s + 0x%lx (%s)", dli.dli_fname, (unsigned long)(r - (u8*)dli.dli_fbase),
             dli.dli_sname ? dli.dli_sname : "?");
  } else {
    snprintf(buf, n, "%p", rip);
  }
}

void watch_segv(int sig, siginfo_t* info, void* vctx) {
  u8* a = (u8*)info->si_addr;
  if (!g_watch_page || a < g_watch_page || a >= g_watch_page + 4096) {
    // no es nuestro: el manejador que hubiera
    if (g_watch_prev_segv.sa_flags & SA_SIGINFO) {
      g_watch_prev_segv.sa_sigaction(sig, info, vctx);
    } else {
      signal(sig, SIG_DFL);
      raise(sig);
    }
    return;
  }
  auto* uc = (ucontext_t*)vctx;
  u32 goal = (u32)(a - g_ee_main_mem);
  g_watch_hit_in_range = goal + 8 > g_watch_addr && goal < g_watch_addr + g_watch_len;
  if (g_watch_hit_in_range) {
    memcpy(&g_watch_before, g_ee_main_mem + (goal & ~3u), 4);
    char where[256];
    watch_describe((void*)uc->uc_mcontext.gregs[REG_RIP], where, sizeof(where));
    char buf[400];
    int n = snprintf(buf, sizeof(buf), "[goal2c] WATCH escritura en 0x%x desde %s\n", goal, where);
    (void)!write(2, buf, (size_t)n);
    if (g_watch_count < 3) {
      auto& g = uc->uc_mcontext.gregs;
      n = snprintf(buf, sizeof(buf),
                   "[goal2c] WATCH   rdi=0x%llx rsi=0x%llx rdx=0x%llx (GOAL: dst 0x%llx src 0x%llx)\n",
                   (unsigned long long)g[REG_RDI], (unsigned long long)g[REG_RSI],
                   (unsigned long long)g[REG_RDX], (unsigned long long)g[REG_RDI],
                   (unsigned long long)g[REG_RSI]);
      (void)!write(2, buf, (size_t)n);
      u64* sp = (u64*)g[REG_RSP];
      for (int i = 0; i < 48; i++) {
        u8* v = (u8*)sp[i];
        if (v >= g_ee_main_mem && v < g_ee_main_mem + EE_MAIN_MEM_SIZE) {
          n = snprintf(buf, sizeof(buf), "[goal2c] WATCH   pila[%d] = GOAL 0x%lx\n", i,
                       (unsigned long)(v - g_ee_main_mem));
          (void)!write(2, buf, (size_t)n);
        } else {
          Dl_info d{};
          if (dladdr(v, &d) && d.dli_fname && strstr(d.dli_fname, "goal2c")) {
            n = snprintf(buf, sizeof(buf), "[goal2c] WATCH   pila[%d] = %s + 0x%lx\n", i,
                         d.dli_fname, (unsigned long)(v - (u8*)d.dli_fbase));
            (void)!write(2, buf, (size_t)n);
          }
        }
      }
    }
  }
  mprotect(g_watch_page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
  uc->uc_mcontext.gregs[REG_EFL] |= 0x100;  // paso a paso: SIGTRAP tras la instrucción
}

void watch_trap(int, siginfo_t*, void* vctx) {
  auto* uc = (ucontext_t*)vctx;
  uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
  if (g_watch_hit_in_range && g_watch_count < 200) {
    g_watch_count++;
    char buf[200];
    u32 after;
    memcpy(&after, g_ee_main_mem + (g_watch_addr & ~3u), 4);
    int n = snprintf(buf, sizeof(buf), "[goal2c] WATCH   palabra 0x%x: 0x%08x -> 0x%08x\n",
                     g_watch_addr & ~3u, g_watch_before, after);
    (void)!write(2, buf, (size_t)n);
  }
  mprotect(g_watch_page, 4096, PROT_READ | PROT_EXEC);
}

void install_watch() {
  const char* w = std::getenv("GOAL2C_WATCH");
  if (!w || g_watch_page) {
    return;
  }
  g_watch_addr = (u32)std::strtoul(w, nullptr, 16);
  const char* comma = strchr(w, ',');
  g_watch_len = comma ? (u32)std::strtoul(comma + 1, nullptr, 0) : 4;
  g_watch_page = (u8*)((uintptr_t)(g_ee_main_mem + g_watch_addr) & ~(uintptr_t)4095);
  struct sigaction sa{};
  sa.sa_sigaction = watch_segv;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &g_watch_prev_segv);
  struct sigaction st{};
  st.sa_sigaction = watch_trap;
  st.sa_flags = SA_SIGINFO;
  sigemptyset(&st.sa_mask);
  sigaction(SIGTRAP, &st, nullptr);
  mprotect(g_watch_page, 4096, PROT_READ | PROT_EXEC);
  lg::info("[goal2c] WATCH activo en 0x{:x} ({} bytes)", g_watch_addr, g_watch_len);
}

void install_segv_handler() {
  struct sigaction sa{};
  sa.sa_sigaction = segv_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &g_prev_segv);
}

//! Carga la librería de GOAL2C_LIB si ha cambiado. Sin la variable, desactiva el modo mixto
//! (los tests arrancan varios runtimes en el mismo proceso, con y sin goal2c).
void update_library() {
  const char* path = std::getenv("GOAL2C_LIB");
  std::string want = path ? path : "";
  if (want == g_loaded_path) {
    return;
  }
  g_files.clear();
  g_replaced.clear();
  g_loaded_path = want;
  if (want.empty()) {
    return;
  }
  void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    lg::error("[goal2c] no se pudo cargar {}: {}", path, dlerror());
    return;
  }
  auto init = (g2c_init_fn)dlsym(lib, "g2c_init");
  if (!init) {
    lg::error("[goal2c] {} no exporta g2c_init", path);
    return;
  }
  static g2c_host host;
  host.mem = g_ee_main_mem;
  host.symbol = host_symbol;
  host.static_addr = host_static;
  host.call = host_call;
  host.pp = &g_pp;
  host.call_q = host_call_q;
  for (auto files = init(&host); *files; files++) {
    g_files[(*files)->obj_name] = *files;
  }
  install_segv_handler();
  lg::info("[goal2c] modo mixto: {} con {} ficheros traducidos", path, g_files.size());
}

//! "(method 3 rgba)" -> (3, "rgba")
bool parse_method(const std::string& name, int* id, std::string* type) {
  if (name.rfind("(method ", 0) != 0 || name.back() != ')') {
    return false;
  }
  auto rest = name.substr(8, name.size() - 9);
  auto space = rest.find(' ');
  if (space == std::string::npos) {
    return false;
  }
  *id = std::atoi(rest.substr(0, space).c_str());
  *type = rest.substr(space + 1);
  return true;
}

/*!
 * Grabador de casos para la comprobación en ARM (GOAL2C_REC=<carpeta>). Builtin GOAL
 * (__g2c-rec cmd a b c), ver test/goal2c/arm/arm_prelude.gc:
 *   0 nombre arena tamaño   empieza: <carpeta>/<nombre>.img = imagen de la memoria GOAL
 *                           [0, cima del heap global) y <nombre>.rec = cabecera + objetos
 *   1 valor                 añade un argumento al próximo caso
 *   2 etiqueta fn flags     empieza un caso: guarda la arena de antes (flags 1 = el retorno no
 *                           cuenta: la función devuelve none)
 *   3 retorno               termina el caso: retorno y arena de después
 *   4                       cierra
 * Formato (little endian) en goalc/c_backend/check/g2c_check.c.
 */
FILE* g_rec = nullptr;
u32 g_rec_arena = 0, g_rec_arena_size = 0;
u64 g_rec_args[8];
std::string g_rec_label, g_rec_fn;
u32 g_rec_nargs = 0, g_rec_flags = 0;
std::vector<u8> g_rec_before;

void rec_u32(u32 v) {
  fwrite(&v, 4, 1, g_rec);
}
void rec_str(const std::string& str) {
  u16 n = (u16)str.size();
  fwrite(&n, 2, 1, g_rec);
  fwrite(str.data(), 1, n, g_rec);
}
std::string goal_string(u32 str) {
  return str ? std::string(Ptr<String>(str)->data()) : std::string();
}
std::string symbol_name(u32 sym) {
  return goal_string(jak1::info(Ptr<jak1::Symbol>(sym))->str.offset);
}

u64 g2c_rec(u64 cmd, u64 a, u64 b, u64 c) {
  const char* dir = std::getenv("GOAL2C_REC");
  switch (cmd) {
    case 0: {
      std::string name = goal_string((u32)a);
      u32 image_size = (kglobalheap->current.offset + 0xffff) & ~0xffffu;
      auto img_path = fmt::format("{}/{}.img", dir, name);
      FILE* img = fopen(img_path.c_str(), "wb");
      if (!img) {
        lg::error("[goal2c] no se puede escribir {}", img_path);
        return 0;
      }
      // la zona baja está protegida (PROT_NONE, para cazar punteros nulos): write() daría
      // EFAULT y la imagen saldría vacía. Se graba a cero.
      std::vector<u8> low(EE_MAIN_MEM_LOW_PROTECT, 0);
      bool ok = fwrite(low.data(), 1, low.size(), img) == low.size() &&
                fwrite(g_ee_main_mem + EE_MAIN_MEM_LOW_PROTECT, 1,
                       image_size - EE_MAIN_MEM_LOW_PROTECT,
                       img) == image_size - EE_MAIN_MEM_LOW_PROTECT;
      fclose(img);
      if (!ok) {
        lg::error("[goal2c] ERROR al escribir {}", img_path);
        return 0;
      }
      g_rec = fopen(fmt::format("{}/{}.rec", dir, name).c_str(), "wb");
      g_rec_arena = (u32)b;
      g_rec_arena_size = (u32)c;
      fwrite("G2CR", 1, 4, g_rec);
      rec_u32(1);  // versión
      rec_u32(image_size);
      rec_u32(s7.offset);
      rec_u32(SymbolTable2.offset);
      rec_u32(LastSymbol.offset);
      rec_u32(jak1::info(Ptr<jak1::Symbol>(s7.offset)).offset - s7.offset);
      rec_u32(*(s7 + FIX_SYM_FUNCTION_TYPE));
      rec_u32(g_rec_arena);
      rec_u32(g_rec_arena_size);
      rec_u32((u32)g_obj_segments.size());
      for (auto& [o, segs] : g_obj_segments) {
        rec_str(o);
        for (u32 v : segs) {
          rec_u32(v);
        }
      }
      lg::info("[goal2c] grabando casos ARM en {}/{}.rec (imagen {} MB)", dir, name,
               image_size >> 20);
      return 1;
    }
    case 1:
      if (g_rec_nargs < 8) {
        g_rec_args[g_rec_nargs] = a;
      }
      g_rec_nargs++;
      return 0;
    case 2:
      g_rec_label = goal_string((u32)a);
      g_rec_fn = symbol_name((u32)b);
      if (g_rec) {
        fflush(g_rec);
      }
      lg::debug("[goal2c] caso ARM {} ({})", g_rec_label, g_rec_fn);
      g_rec_flags = (u32)c;
      g_rec_before.assign(g_ee_main_mem + g_rec_arena,
                          g_ee_main_mem + g_rec_arena + g_rec_arena_size);
      return 0;
    case 3:
      if (g_rec) {
        fwrite("CASE", 1, 4, g_rec);
        rec_str(g_rec_label);
        rec_str(g_rec_fn);
        rec_u32(g_rec_nargs);
        rec_u32(g_rec_flags);
        fwrite(g_rec_args, 8, 8, g_rec);
        fwrite(&a, 8, 1, g_rec);
        fwrite(g_rec_before.data(), 1, g_rec_before.size(), g_rec);
        fwrite(g_ee_main_mem + g_rec_arena, 1, g_rec_arena_size, g_rec);
      }
      memset(g_rec_args, 0, sizeof(g_rec_args));
      g_rec_nargs = 0;
      return 0;
    case 4:
      if (g_rec) {
        fwrite("END!", 1, 4, g_rec);
        fclose(g_rec);
        g_rec = nullptr;
      }
      return 0;
    default:
      return 0;
  }
}

/*!
 * Suspend nativo (modo mixto). El thread-suspend de gkernel.gc copia la pila usada del proceso
 * ([sp, stack-top), en *dram-stack*) al área de respaldo del cpu-thread, de 256 bytes
 * (PROCESS_STACK_SAVE_SIZE); con marcos de C y trampolines no cabe. Aquí se sustituyen los
 * métodos thread-suspend y thread-resume de cpu-thread (10 y 11) por g2c_native_suspend /
 * g2c_native_resume: mismo modelo (todos los procesos corren en la misma pila; al suspender se
 * copia la parte usada y al reanudar se devuelve a la MISMA dirección, así los punteros a la
 * pila de los marcos de C siguen valiendo), pero la copia va a un búfer del host, sin límite.
 * - Un búfer por suspensión: se crea al suspender y se libera al reanudar. El área de respaldo
 *   del hilo guarda {marca, id, cookie}: viaja con el proceso si se reubica. La cookie (única
 *   por suspensión) evita que un área vieja (de un proceso muerto o ya reubicado) se confunda
 *   con un búfer vivo que haya reutilizado el id.
 * - Un proceso que muere suspendido nunca se reanuda: deactivate (método 10 de process) pasa
 *   por g2c_deactivate_hook, que libera su búfer y llama al deactivate original.
 * Solo con la librería de goal2c cargada (las ejecuciones todo x86 siguen con el original).
 * GOAL2C_SUSPEND_X86=1 lo desactiva (y las funciones que escriben pp se quedan en x86).
 */
constexpr u32 kSuspendMagic = 0x53433247;  // "G2CS"
struct SuspendBuf {
  u32 cookie = 0;  // 0 = libre
  std::vector<u8> data;
};
std::vector<SuspendBuf> g_susp;
std::vector<u32> g_susp_free;
u32 g_susp_cookie = 0;
u32 g_susp_live = 0, g_susp_live_max = 0;
size_t g_susp_bytes_max = 0;
u64 g_susp_count = 0;
bool g_suspend_native = false;
u32 g_sym_kernel_sp = 0, g_sym_suspended = 0, g_sym_running = 0;
u32 g_deactivate_orig = 0;

template <typename T>
T& goal_field(u32 obj, u32 off) {
  return *(T*)(g_ee_main_mem + obj + off);
}

void susp_free(u32 id) {
  g_susp[id].cookie = 0;
  g_susp[id].data.clear();  // conserva la capacidad
  g_susp_free.push_back(id);
  g_susp_live--;
}

//! Libera el búfer del hilo si el área apunta a una suspensión viva (cookie). Devuelve el id.
int susp_lookup(u32 thread) {
  u32 magic = goal_field<u32>(thread, 124), id = goal_field<u32>(thread, 128),
      cookie = goal_field<u32>(thread, 132);
  if (magic != kSuspendMagic || id >= g_susp.size() || g_susp[id].cookie == 0 ||
      g_susp[id].cookie != cookie) {
    return -1;
  }
  return (int)id;
}

u64 deactivate_hook(u64 proc) {
  u32 p = (u32)proc;
  u32 thread = p ? goal_field<u32>(p, 40) : 0;  // main-thread
  if (thread) {
    int id = susp_lookup(thread);
    if (id >= 0) {
      susp_free((u32)id);
      goal_field<u32>(thread, 124) = 0;
    }
  }
  // sin destructores aquí: deactivate del proceso actual no vuelve (salta a return-from-thread)
  return host_call(g_deactivate_orig, 1, &proc);
}

//! Objeto función GOAL con  movabs rax, target ; jmp rax  (para entrar en el asm nativo)
u32 make_jmp_entry(void* target) {
  u32 slot = g_entry_next;
  g_entry_next += kEntrySlot;
  memset(Ptr<u8>(slot).c(), 0, kEntrySlot);
  *Ptr<u32>(slot + 16).c() = *(s7 + FIX_SYM_FUNCTION_TYPE);
  auto mem = Ptr<u8>(slot + 20);
  int off = 0;
  emit_movabs_rax(mem.c(), &off, (u64)target);
  mem.c()[off++] = 0xff;
  mem.c()[off++] = 0xe0;
  flush_icache_goal(mem.offset, off);
  return mem.offset;
}

u32 symbol_value(const char* name) {
  auto sym = jak1::find_symbol_from_c(name);
  return sym.offset ? *Ptr<u32>(sym.offset).c() : 0;
}

//! Tras enlazar gkernel (tipos cpu-thread y process y sus métodos ya definidos).
void install_native_suspend() {
  g_susp.clear();
  g_susp_free.clear();
  g_susp_live = g_susp_live_max = 0;
  g_susp_bytes_max = 0;
  g_susp_count = 0;
  g_suspend_native = false;
  g_suspend_native_on = false;
  g_format_fn = 0;
  if (std::getenv("GOAL2C_SUSPEND_X86")) {
    lg::info("[goal2c] suspend: el de x86 (GOAL2C_SUSPEND_X86)");
    return;
  }
  u32 cpu_thread = symbol_value("cpu-thread");
  u32 process = symbol_value("process");
  g_sym_kernel_sp = jak1::find_symbol_from_c("*kernel-sp*").offset;
  g_sym_suspended = jak1::find_symbol_from_c("suspended").offset;
  g_sym_running = jak1::find_symbol_from_c("running").offset;
  if (!cpu_thread || !process || !g_sym_kernel_sp || !g_sym_suspended || !g_sym_running) {
    lg::error("[goal2c] suspend nativo: faltan tipos o símbolos del kernel; se queda el de x86");
    return;
  }
  jak1::method_set(cpu_thread, 10, make_jmp_entry((void*)g2c_native_suspend));
  jak1::method_set(cpu_thread, 11, make_jmp_entry((void*)g2c_native_resume));
  g_deactivate_orig = Ptr<jak1::Type>(process)->get_method(10).offset;
  jak1::method_set(process, 10, make_c_entry((void*)deactivate_hook, Entry::ARGS));
  g_suspend_native = true;
  g_suspend_native_on = true;
  g_format_fn = symbol_value("format");
  lg::info("[goal2c] suspend nativo instalado (copia de pila fuera del heap)");
}

}  // namespace
}  // namespace goal2c_runtime

extern "C" u64 g2c_suspend_save(u64 thread64, u64 rsp) {
  using namespace goal2c_runtime;
  u32 thread = (u32)thread64;
  u32 sp = (u32)(rsp - (u64)g_ee_main_mem);
  u32 top = goal_field<u32>(thread, 28);
  if (top < sp || top - sp > 0x100000 || goal_field<s32>(thread, 32) < 12) {
    lg::die("[goal2c] suspend nativo: hilo 0x{:x} con sp 0x{:x} y stack-top 0x{:x}", thread, sp,
            top);
  }
  u32 id;
  if (!g_susp_free.empty()) {
    id = g_susp_free.back();
    g_susp_free.pop_back();
  } else {
    id = (u32)g_susp.size();
    g_susp.emplace_back();
  }
  if (++g_susp_cookie == 0) {
    g_susp_cookie = 1;
  }
  auto& b = g_susp[id];
  b.cookie = g_susp_cookie;
  b.data.assign(g_ee_main_mem + sp, g_ee_main_mem + top);
  goal_field<u32>(thread, 124) = kSuspendMagic;
  goal_field<u32>(thread, 128) = id;
  goal_field<u32>(thread, 132) = b.cookie;
  g_susp_live++;
  g_susp_count++;
  g_susp_live_max = std::max(g_susp_live_max, g_susp_live);
  g_susp_bytes_max = std::max(g_susp_bytes_max, b.data.size());
  if ((g_susp_count & 0x3ffff) == 0) {
    lg::info("[goal2c] suspend nativo: {} suspensiones, {} vivas (máx. {}), pila máx. {} bytes",
             g_susp_count, g_susp_live, g_susp_live_max, g_susp_bytes_max);
  }
  // proceso suspendido
  goal_field<u32>(goal_field<u32>(thread, 4), 32) = g_sym_suspended;
  return (u64)g_ee_main_mem + *Ptr<u32>(g_sym_kernel_sp).c();
}

extern "C" u64 g2c_resume_restore(u64 thread64, u64 kernel_rsp) {
  using namespace goal2c_runtime;
  u32 thread = (u32)thread64;
  *Ptr<u32>(g_sym_kernel_sp).c() = (u32)(kernel_rsp - (u64)g_ee_main_mem);
  u32 sp = goal_field<u32>(thread, 24);
  u32 top = goal_field<u32>(thread, 28);
  u64 restored = 0;
  if (sp < top) {
    restored = 1;
    int id = susp_lookup(thread);
    if (id < 0 || g_susp[id].data.size() != top - sp) {
      lg::die("[goal2c] resume nativo: el hilo 0x{:x} (sp 0x{:x}, stack-top 0x{:x}) no tiene "
              "una pila guardada válida",
              thread, sp, top);
    }
    memcpy(g_ee_main_mem + sp, g_susp[id].data.data(), top - sp);
    susp_free((u32)id);
    goal_field<u32>(thread, 124) = 0;
  }
  u32 proc = goal_field<u32>(thread, 4);
  goal_field<u32>(proc, 44) = thread;          // top-thread
  goal_field<u32>(proc, 32) = g_sym_running;  // status
  return restored;
}

namespace goal2c_runtime {

void after_link_jak1(const char* object_name, ObjectFileHeader* ofh) {
  update_library();
  std::string obj = object_name;
  auto slash = obj.find_last_of("/\\");
  if (slash != std::string::npos) {
    obj = obj.substr(slash + 1);
  }
  g_obj_segments[obj] = {ofh->code_infos[0].offset, ofh->code_infos[0].size,
                         ofh->code_infos[1].offset, ofh->code_infos[1].size,
                         ofh->code_infos[2].offset, ofh->code_infos[2].size};
  if (obj == "gcommon") {
    // primer objeto del kernel: arranque de un runtime nuevo (los tests arrancan varios)
    g_obj_segments.clear();
    install_watch();
    if (std::getenv("GOAL2C_REC")) {
      jak1::make_function_symbol_from_c("__g2c-rec", (void*)g2c_rec);
    }
    g_entry_next = kEntryZoneStart + kEntrySlot;
    memset(Ptr<u8>(kEntryZoneStart).c(), 0, kEntrySlot);
    *Ptr<u32>(kDummySymbol).c() = s7.offset;  // valor #f
    {  // rampa C -> GOAL: call rax ; ret
      u8* pad = Ptr<u8>(kEntryZoneStart + 0x30).c();
      pad[0] = 0xff;
      pad[1] = 0xd0;
      pad[2] = 0xc3;
      flush_icache_goal(kEntryZoneStart + 0x30, 3);
      g2c_goal_call_pad = (u64)pad;
    }
    g_fingerprint_frame = 0;
    g_vclock_in_frame = 0;
    g_script_start = -1;
    g_salto_hecho = false;
    g_salto_name.clear();
    g_salto_at = 600;
    g_progreso_tareas.clear();
    g_progreso_celulas = -1;
    g_salto_env_read = false;
    det_sound_reset();
    if (deterministic_mode()) {
      install_segv_handler();  // también en x86 puro (diagnóstico de la huella y la grabación)
    }
  }
  if (g_files.empty()) {
    return;
  }
  if (obj == "gkernel") {
    install_native_suspend();
  }
  auto it = g_files.find(obj);
  if (it == g_files.end()) {
    return;
  }
  const g2c_file_info* info = it->second;

  // el objeto cargado debe ser el mismo que se transpiló. El segmento de depuración solo se
  // carga con -debug: si no está (size 0), no se compara (sus estáticos no existirán).
  for (int seg = 0; seg < N_SEG; seg++) {
    if (seg == DEBUG_SEGMENT && ofh->code_infos[seg].size == 0) {
      continue;
    }
    if (ofh->code_infos[seg].size != info->seg_sizes[seg]) {
      lg::warn(
          "[goal2c] {}: el objeto cargado no coincide con el transpilado (segmento {}: {} vs "
          "{} bytes); se queda en x86",
          obj, seg, ofh->code_infos[seg].size, info->seg_sizes[seg]);
      return;
    }
  }

  g_cur_ofh = ofh;
  g_cur_obj = obj;
  info->link();
  g_cur_ofh = nullptr;

  const u32 function_type = *(s7 + FIX_SYM_FUNCTION_TYPE);
  int replaced = 0, skipped = 0, patched = 0, kept_pp = 0;
  for (auto* e = info->funcs; e->name; e++) {
    // diagnóstico: GOAL2C_PP_X86 deja en x86 todas las que escriben pp; GOAL2C_PP_SOLO=a,b solo
    // instala en C las de esos objetos (para bisecar)
    static const bool pp_x86 = std::getenv("GOAL2C_PP_X86") != nullptr;
    static const char* pp_solo = std::getenv("GOAL2C_PP_SOLO");
    bool pp_excluded = pp_x86 || (pp_solo && !strstr(("," + std::string(pp_solo) + ",").c_str(),
                                                     ("," + obj + ",").c_str()));
    static const char* fn_x86 = std::getenv("GOAL2C_FN_X86");  // diagnóstico: nombre,nombre
    if (fn_x86 && strstr(("," + std::string(fn_x86) + ",").c_str(),
                         ("," + std::string(e->name) + ",").c_str())) {
      skipped++;
      continue;
    }
    if ((e->flags & G2C_FN_WRITES_PP) && (!g_suspend_native || pp_excluded)) {
      kept_pp++;  // suspend / with-proc con el suspend de x86: se queda en x86
      continue;
    }
    if (e->seg == DEBUG_SEGMENT && ofh->code_infos[DEBUG_SEGMENT].size == 0) {
      skipped++;  // segmento de depuración no cargado (sin -debug): la función no existe
      continue;
    }
    Entry kind = e->fn_q ? Entry::Q : (e->fn_arr ? Entry::STACK : Entry::ARGS);
    void* fn = e->fn_q ? e->fn_q : (e->fn_arr ? e->fn_arr : e->fn);
    std::string name = e->name;
    int method_id;
    std::string type_name;
    bool installed = false;
    if (parse_method(name, &method_id, &type_name)) {
      auto type_sym = jak1::find_symbol_from_c(type_name.c_str());
      u32 type = type_sym.offset ? *Ptr<u32>(type_sym.offset).c() : 0;
      if (type) {
        jak1::method_set(type, method_id, make_c_entry(fn, kind));
        installed = true;
      } else {
        lg::warn("[goal2c] {}: tipo {} sin definir, no se sustituye {}", obj, type_name, name);
      }
    } else if (auto fsym = jak1::find_symbol_from_c(name.c_str()); fsym.offset) {
      // (find, no intern: no crear símbolos ni strings en el heap)
      u32* value = Ptr<u32>(fsym.offset).c();
      // solo si el top-level dejó ahí una función (inline, etc. no se tocan)
      if (*value != 0 && *Ptr<u32>(*value - 4).c() == function_type) {
        *value = make_c_entry(fn, kind);
        installed = true;
      }
    }
    if (installed) {
      replaced++;
      continue;
    }
    // Sin símbolo ni método (lambdas: estados, behaviors...): parchear el código x86 in situ
    // con  movabs rax, entrada ; jmp rax  (12 bytes). Quien tenga la dirección de la función
    // (estados, otras funciones, el C con mov-fa) entra así en el C.
    if (e->seg >= 0 && e->seg < N_SEG && e->length >= 12 && ofh->code_infos[e->seg].size != 0) {
      u32 entry = make_c_entry(fn, kind);
      u32 at = ofh->code_infos[e->seg].offset + (u32)e->offset;
      u8* code = Ptr<u8>(at).c();
      int off = 0;
      emit_movabs_rax(code, &off, (u64)(g_ee_main_mem + entry));
      code[off++] = 0xff;  // jmp rax
      code[off++] = 0xe0;
      flush_icache_goal(at, off);
      replaced++;
      patched++;
      continue;
    }
    lg::debug("[goal2c] {}: {} no tiene símbolo ni método y no se puede parchear", obj, name);
    skipped++;
  }
  g_replaced[obj] = replaced;
  lg::info(
      "[goal2c] {}: {} funciones en C ({} parcheadas in situ), {} se quedan en x86 ({} por "
      "escribir pp)",
      obj, replaced, patched, skipped + kept_pp, kept_pp);
}

int replaced_functions(const char* object_name) {
  auto it = g_replaced.find(object_name);
  return it == g_replaced.end() ? 0 : it->second;
}

namespace {
constexpr u64 kTicksPerFrame = 300000000ULL / 60;  // __read-ee-timer: 300 MHz
}  // namespace

//! Pone a cero, en una copia de paquetes GIF, los campos que el GS no usa: la 4.ª palabra de
//! los registros STQ (descriptor 0x2) en modo PACKED.
void mask_gif_ignored(u8* data, u32 size) {
  u32 off = 0;
  while (off + 16 <= size) {
    u64 lo, hi;
    memcpy(&lo, data + off, 8);
    memcpy(&hi, data + off + 8, 8);
    off += 16;
    u32 nloop = lo & 0x7fff;
    u32 flg = (lo >> 58) & 3;
    u32 nreg = (lo >> 60) & 0xf;
    if (nreg == 0) {
      nreg = 16;
    }
    if (flg == 0) {  // PACKED: nloop * nreg qwords
      for (u32 l = 0; l < nloop; l++) {
        for (u32 r = 0; r < nreg; r++) {
          if (off + 16 > size) {
            return;
          }
          u32 desc = (hi >> (4 * r)) & 0xf;
          if (desc == 0x2) {
            memset(data + off + 12, 0, 4);
          }
          off += 16;
        }
      }
    } else if (flg == 1) {  // REGLIST: nloop * nreg palabras de 64 bits, alineado a 128
      off += ((nloop * nreg * 8) + 15) & ~15u;
    } else {  // IMAGE / desactivado: nloop qwords
      off += nloop * 16;
    }
  }
}

//! En datos de UNPACK V4-32: pone a cero la 4.ª palabra de los qwords cuya 3.ª es 1.0f (vectores
//! S, T, Q=1 que el VU usa como STQ). GOAL los construye con movss sobre un registro cuyo resto
//! es indefinido: en x86 queda lo que dejó la operación anterior, en C un 0.
void mask_unpack_stq_w(u8* data, u32 size) {
  for (u32 off = 0; off + 16 <= size; off += 16) {
    u32 z;
    memcpy(&z, data + off + 8, 4);
    if (z == 0x3f800000) {
      memset(data + off + 12, 0, 4);
    }
  }
}

bool virtual_ee_timer(unsigned long long* ticks) {
  if (!deterministic_mode()) {
    return false;
  }
  // 100 ticks (1/3 de microsegundo) por lectura, sin pasar al fotograma siguiente
  if (g_vclock_in_frame + 100 < kTicksPerFrame) {
    g_vclock_in_frame += 100;
  }
  *ticks = g_fingerprint_frame * kTicksPerFrame + g_vclock_in_frame;
  return true;
}

namespace {
// Recorrido: una línea por tramo de fotogramas con la misma entrada (ver test/goal2c/recorrido.txt)
//   <duración> <botones> <lx> <ly> [<rx> <ry> [<presión x12>]]
// y líneas de control "# sync <t> <x> <y> <z>" (posición de Jak en el fotograma t, en hex de
// los bits del float; "-" si no hay *target*). "# sin-repetir": al acabar, mando en reposo.
struct PadStep {
  u64 from, to;  // [from, to) en fotogramas desde que existe *target*
  u16 buttons;
  u8 lx, ly, rx, ry;
  u8 press[12];
};
struct SyncPoint {
  u64 t;
  bool has_target;
  u32 x, y, z;
};
std::vector<PadStep> g_script;
std::vector<SyncPoint> g_sync;
size_t g_sync_next = 0;
u64 g_script_len = 0;
bool g_script_repeat = true;
bool g_script_loaded = false;
int g_sync_ok = 0, g_sync_bad = 0;

const char* kButtonNames[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                "l2", "r2", "l1", "r1", "triangle", "circle", "x", "square"};

u16 parse_buttons(const std::string& txt) {
  u16 b = 0;
  size_t pos = 0;
  while (pos <= txt.size()) {
    size_t end = txt.find('+', pos);
    std::string name = txt.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    for (int i = 0; i < 16; i++) {
      if (name == kButtonNames[i]) {
        b |= (u16)(1 << i);
      }
    }
    if (end == std::string::npos) {
      break;
    }
    pos = end + 1;
  }
  return b;
}

std::string format_buttons(u16 b) {
  std::string out;
  for (int i = 0; i < 16; i++) {
    if (b & (1 << i)) {
      out += (out.empty() ? "" : "+") + std::string(kButtonNames[i]);
    }
  }
  return out.empty() ? "-" : out;
}

void load_script(const char* path) {
  g_script.clear();
  g_sync.clear();
  g_sync_next = 0;
  g_script_len = 0;
  g_script_repeat = true;
  FILE* f = fopen(path, "r");
  if (!f) {
    lg::die("[recorrido] no se puede leer {}", path);
  }
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#') {
      unsigned long long t;
      unsigned x, y, z;
      char dash[4];
      if (sscanf(line, "# sync %llu %x %x %x", &t, &x, &y, &z) == 4) {
        g_sync.push_back({t, true, x, y, z});
      } else if (sscanf(line, "# sync %llu %1s", &t, dash) == 2 && dash[0] == '-') {
        g_sync.push_back({t, false, 0, 0, 0});
      } else if (strncmp(line, "# sin-repetir", 13) == 0) {
        g_script_repeat = false;
      } else if (strncmp(line, "# salto ", 8) == 0) {
        char sname[64];
        unsigned long long at = 600;
        salto_config_from_env();
        if (g_salto_name.empty() && sscanf(line, "# salto %63s %llu", sname, &at) >= 1) {
          g_salto_name = sname;  // la grabación arranca en un nivel: el salto se repite
          g_salto_at = at;
        }
      } else if (strncmp(line, "# progreso", 10) == 0) {
        g_progreso_tareas.clear();
        for (const char* c = line + 10; *c;) {
          while (*c == ' ') c++;
          if (*c < '0' || *c > '9') break;
          g_progreso_tareas.push_back(std::atoi(c));
          while (*c >= '0' && *c <= '9') c++;
        }
      } else if (strncmp(line, "# celulas ", 10) == 0) {
        g_progreso_celulas = std::atoi(line + 10);
      }
      continue;
    }
    unsigned long long dur;
    char buttons[160];
    unsigned v[16];
    int n = sscanf(line,
                   "%llu %159s %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u", &dur, buttons,
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10],
                   &v[11], &v[12], &v[13], &v[14], &v[15]);
    if (n < 4) {
      continue;
    }
    PadStep st{};
    st.from = g_script_len;
    st.to = g_script_len + dur;
    st.buttons = parse_buttons(buttons);
    st.lx = (u8)v[0];
    st.ly = (u8)v[1];
    st.rx = n >= 6 ? (u8)v[2] : 128;
    st.ry = n >= 6 ? (u8)v[3] : 128;
    for (int i = 0; i < 12; i++) {
      st.press[i] = n >= 18 ? (u8)v[4 + i] : 0;
    }
    g_script.push_back(st);
    g_script_len += dur;
  }
  fclose(f);
  lg::info("[recorrido] {}: {} tramos, {} fotogramas, {} puntos de sincronía{}", path,
           g_script.size(), g_script_len, g_sync.size(),
           g_script_repeat ? " (se repite)" : "");
}

//! posición de Jak (trans de la raíz de *target*), en bits; false si no hay *target*
bool target_pos(u32 out[3]) {
  auto target = jak1::find_symbol_from_c("*target*");
  u32 proc = target.offset ? *Ptr<u32>(target.offset).c() : s7.offset;
  if (proc == s7.offset || proc == 0) {
    return false;
  }
  u32 root = *Ptr<u32>(proc - 4 + 112).c();  // process-drawable root (trsqv)
  if (root == 0 || root == s7.offset) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    out[i] = *Ptr<u32>(root - 4 + 16 + 4 * i).c();  // trs trans
  }
  return true;
}

bool target_exists() {
  auto target = jak1::find_symbol_from_c("*target*");
  u32 value = target.offset ? *Ptr<u32>(target.offset).c() : s7.offset;
  return value != s7.offset && value != 0;
}

// ---- grabación (GOAL2C_GRABAR) ----
FILE* g_rec_pad = nullptr;
PadStep g_rec_cur{};
bool g_rec_have = false;
u64 g_rec_last_frame = ~0ULL;

void rec_flush_step() {
  if (!g_rec_have || !g_rec_pad) {
    return;
  }
  auto& st = g_rec_cur;
  fprintf(g_rec_pad, "%llu %s %u %u %u %u", (unsigned long long)(st.to - st.from),
          format_buttons(st.buttons).c_str(), st.lx, st.ly, st.rx, st.ry);
  for (int i = 0; i < 12; i++) {
    fprintf(g_rec_pad, " %u", st.press[i]);
  }
  fprintf(g_rec_pad, "\n");
  fflush(g_rec_pad);
}

void record_pad(const CPadInfo* pad, u64 t) {
  if (t == g_rec_last_frame) {
    return;  // una muestra por fotograma
  }
  g_rec_last_frame = t;
  PadStep st{};
  bool connected = (pad->valid & 0x80) == 0;
  st.buttons = connected ? pad->button0 : 0;
  st.lx = connected ? pad->leftx : 128;
  st.ly = connected ? pad->lefty : 128;
  st.rx = connected ? pad->rightx : 128;
  st.ry = connected ? pad->righty : 128;
  for (int i = 0; i < 12; i++) {
    st.press[i] = connected ? pad->abutton[i] : 0;
  }
  bool same = g_rec_have && st.buttons == g_rec_cur.buttons && st.lx == g_rec_cur.lx &&
              st.ly == g_rec_cur.ly && st.rx == g_rec_cur.rx && st.ry == g_rec_cur.ry &&
              memcmp(st.press, g_rec_cur.press, 12) == 0;
  if (same) {
    g_rec_cur.to = t + 1;
  } else {
    rec_flush_step();
    st.from = t;
    st.to = t + 1;
    g_rec_cur = st;
    g_rec_have = true;
  }
  if (t % 60 == 0) {  // control de sincronía cada segundo de juego
    rec_flush_step();
    g_rec_have = false;
    u32 p[3];
    if (target_pos(p)) {
      fprintf(g_rec_pad, "# sync %llu %08x %08x %08x\n", (unsigned long long)t, p[0], p[1], p[2]);
    } else {
      fprintf(g_rec_pad, "# sync %llu -\n", (unsigned long long)t);
    }
    fflush(g_rec_pad);
  }
}

void grab_pad(const char* path, const CPadInfo* pad, u64 t) {
  if (!g_rec_pad) {
    g_rec_pad = fopen(path, "w");
    if (!g_rec_pad) {
      lg::die("[grabación] no se puede escribir {}", path);
    }
    fprintf(g_rec_pad,
            "# goal2c (Jak Vita): recorrido grabado (task boot-game GRABAR=<fichero>).\n"
            "# Formato: duración botones lx ly rx ry presión x12; # sync = posición de Jak.\n"
            "# sin-repetir\n");
    salto_config_from_env();
    if (!g_salto_name.empty()) {
      fprintf(g_rec_pad, "# salto %s %llu\n", g_salto_name.c_str(),
              (unsigned long long)g_salto_at);
      if (!g_progreso_tareas.empty()) {
        fprintf(g_rec_pad, "# progreso");
        for (int t : g_progreso_tareas) {
          fprintf(g_rec_pad, " %d", t);
        }
        fprintf(g_rec_pad, "\n");
      }
      if (g_progreso_celulas >= 0) {
        fprintf(g_rec_pad, "# celulas %d\n", g_progreso_celulas);
      }
    }
    lg::info("[grabación] grabando el mando 0 en {}", path);
  }
  record_pad(pad, t);
}

u64 salto_fn() {
  const char* name = g_salto_name.c_str();
  u32 gi = symbol_value("*game-info*");
  u32 start = symbol_value("start");
  auto play = jak1::find_symbol_from_c("play");
  if (!*name || !gi || !start || !play.offset) {
    lg::error("[salto] faltan *game-info*, start o 'play");
    return s7.offset;
  }
  u32 gi_type = *Ptr<u32>(gi - 4).c();
  u32 get_cont = Ptr<jak1::Type>(gi_type)->get_method(18).offset;  // get-continue-by-name
  u64 a[2] = {gi, g_salto_string};
  u64 cont = host_call(get_cont, 2, a);
  if ((u32)cont == s7.offset || (u32)cont == 0) {
    lg::error("[salto] punto de continuación {} desconocido", name);
    return s7.offset;
  }
  lg::info("[salto] fotograma {}: (start 'play {})", g_fingerprint_frame, name);
  u64 b[2] = {play.offset, cont};
  u64 ret = host_call(start, 2, b);
  // progreso necesario para la mecánica del nivel (flut flut, zoomer...)
  u32 close_task = symbol_value("close-specific-task!");
  for (int task : g_progreso_tareas) {
    for (u64 status = 2; status <= 7; status++) {  // need-hint .. need-resolution
      u64 c[2] = {(u64)task, status};
      host_call(close_task, 2, c);
    }
  }
  if (g_progreso_celulas >= 0) {
    *Ptr<float>(gi - 4 + 96).c() = (float)g_progreso_celulas;  // game-info fuel
  }
  if (!g_progreso_tareas.empty() || g_progreso_celulas >= 0) {
    lg::info("[salto] progreso: {} tareas cerradas, {} células", g_progreso_tareas.size(),
             g_progreso_celulas);
  }
  return ret;
}
}  // namespace

void before_kernel_dispatch() {
  salto_config_from_env();
  if (g_salto_name.empty() || g_salto_hecho || !deterministic_mode()) {
    return;
  }
  const char* name = g_salto_name.c_str();
  if (g_script_start < 0) {  // sin recorrido: el fotograma en que aparece Jak
    if (!target_exists()) {
      return;
    }
    g_script_start = (s64)g_fingerprint_frame;
  }
  if (g_fingerprint_frame < (u64)g_script_start + g_salto_at) {
    return;
  }
  g_salto_hecho = true;
  // string GOAL con el nombre, en la zona de entradas (fuera del heap): basic en 4 mod 8
  size_t len = strlen(name);
  u32 slot = g_entry_next;
  g_entry_next += (u32)((20 + len + 1 + kEntrySlot - 1) / kEntrySlot * kEntrySlot);
  memset(Ptr<u8>(slot).c(), 0, 20 + len + 1);
  *Ptr<u32>(slot + 16).c() = *(s7 + FIX_SYM_STRING_TYPE);
  *Ptr<u32>(slot + 20).c() = (u32)len;
  memcpy(Ptr<u8>(slot + 24).c(), name, len);
  g_salto_string = slot + 20;
  // como un envío del REPL: el kernel la ejecuta en el proceso del listener
  jak1::ListenerFunction->value = make_c_entry((void*)salto_fn, Entry::ARGS);
}

bool deterministic_mode() {
  static const bool on =
      std::getenv("GOAL2C_HUELLA") != nullptr || std::getenv("GOAL2C_GRABAR") != nullptr;
  return on;
}

void scripted_pad(unsigned long long cpad_info) {
  static const char* replay = std::getenv("GOAL2C_RECORRIDO");
  static const char* grab = std::getenv("GOAL2C_GRABAR");
  if (!deterministic_mode() || (!(replay && *replay) && !(grab && *grab))) {
    return;
  }
  auto pad = Ptr<CPadInfo>((u32)cpad_info).c();
  if (pad->number != 0) {
    return;
  }
  if (g_script_start < 0) {
    if (!target_exists()) {
      if (replay && *replay) {  // reproducción: mando en reposo hasta que exista Jak
        pad->valid = 0;
        pad->status = 0x73;
        pad->button0 = 0;
        pad->leftx = pad->lefty = pad->rightx = pad->righty = 128;
        memset(pad->abutton, 0, sizeof(pad->abutton));
      }
      return;
    }
    g_script_start = (s64)g_fingerprint_frame;
    lg::info("[recorrido] empieza en el fotograma {}", g_fingerprint_frame);
  }
  u64 t = g_fingerprint_frame - (u64)g_script_start;
  // con salto de nivel: mando en reposo hasta que se haya hecho (el juego sigue en el título)
  salto_config_from_env();
  const bool salto_pendiente = !g_salto_name.empty() && !g_salto_hecho;

  if (!(replay && *replay)) {  // solo grabación: el mando real
    if (salto_pendiente) {
      pad->button0 = 0;
      pad->leftx = pad->lefty = pad->rightx = pad->righty = 128;
      memset(pad->abutton, 0, sizeof(pad->abutton));
    }
    grab_pad(grab, pad, t);
    return;
  }
  if (!g_script_loaded) {
    load_script(replay);
    g_script_loaded = true;
  }
  // control de sincronía: la posición de Jak debe ser la grabada, bit a bit
  while (g_sync_next < g_sync.size() && g_sync[g_sync_next].t < t) {
    g_sync_next++;
  }
  if (g_sync_next < g_sync.size() && g_sync[g_sync_next].t == t) {
    auto& sp = g_sync[g_sync_next++];
    u32 p[3];
    bool has = target_pos(p);
    bool ok = has == sp.has_target && (!has || (p[0] == sp.x && p[1] == sp.y && p[2] == sp.z));
    if (ok) {
      g_sync_ok++;
    } else if (g_sync_bad++ == 0) {
      lg::warn("[recorrido] DESINCRONIZADO en t={}: Jak en {:08x} {:08x} {:08x}, grabado {:08x} "
               "{:08x} {:08x}",
               t, has ? p[0] : 0, has ? p[1] : 0, has ? p[2] : 0, sp.x, sp.y, sp.z);
    }
    if (g_sync_next == g_sync.size() || g_sync_next % 60 == 0) {
      lg::info("[recorrido] sincronía: {} puntos bien, {} mal", g_sync_ok, g_sync_bad);
    }
  }
  pad->valid = 0;
  pad->status = 0x73;
  pad->button0 = 0;
  pad->leftx = pad->lefty = pad->rightx = pad->righty = 128;
  memset(pad->abutton, 0, sizeof(pad->abutton));
  if (g_script_len != 0 && (g_script_repeat || t < g_script_len)) {
    u64 tt = t % g_script_len;
    for (auto& st : g_script) {
      if (tt >= st.from && tt < st.to) {
        pad->button0 = st.buttons;
        pad->leftx = st.lx;
        pad->lefty = st.ly;
        pad->rightx = st.rx;
        pad->righty = st.ry;
        memcpy(pad->abutton, st.press, 12);
        break;
      }
    }
  }
  if (salto_pendiente) {
    pad->button0 = 0;
    pad->leftx = pad->lefty = pad->rightx = pad->righty = 128;
    memset(pad->abutton, 0, sizeof(pad->abutton));
  }
  // con GOAL2C_GRABAR además: graba la entrada inyectada (prueba de la grabación sin ventana)
  if (grab && *grab) {
    grab_pad(grab, pad, t);
  }
}

void frame_fingerprint(u32 chain) {
  static const bool enabled = std::getenv("GOAL2C_HUELLA") != nullptr;
  det_sound_frame();  // modo determinista: *sound-iop-info* del fotograma
  if (!enabled) {
    if (deterministic_mode()) {  // grabación: reloj virtual y contador, sin hash
      g_vclock_in_frame = 0;
      g_fingerprint_frame++;
    }
    return;
  }
  g_vclock_in_frame = 0;
  // FNV-1a de 64 bits sobre cada transferencia (tag de VIF y datos), siguiendo la cadena
  // dos hashes: estricto (todo) y enmascarado (sin los campos que el hardware ignora y que el
  // código original deja sin inicializar; ver mask_gif_ignored / mask_unpack_stq_w)
  u64 h = 0xcbf29ce484222325ULL, hs = 0xcbf29ce484222325ULL;
  auto mix_one = [](u64& acc, const u8* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
      acc = (acc ^ p[i]) * 0x100000001b3ULL;
    }
  };
  auto mix = [&](const u8* p, size_t n) { mix_one(h, p, n); };
  auto mix_strict = [&](const u8* p, size_t n) { mix_one(hs, p, n); };
  // GOAL2C_HUELLA_VOLCADO=<n>: escribe el fotograma n (transferencias seguidas) en un fichero,
  // para comparar dos ejecuciones con cmp y ver qué cambia
  static const char* dump_env = std::getenv("GOAL2C_HUELLA_VOLCADO");
  FILE* dump = nullptr;
  if (dump_env && (u64)std::atoll(dump_env) == g_fingerprint_frame) {
    auto path = fmt::format("huella_{}_{}.bin", g_fingerprint_frame, getpid());
    dump = fopen(path.c_str(), "wb");
    lg::info("[huella] volcado en {}", path);
  }
  // Pila de los procesos (*dram-stack*, 32 KB en el heap global). Algunas estructuras se crean
  // con (new 'stack ...) y la cadena DMA las referencia ahí mismo: cuando se envía la cadena, el
  // resto de esa zona tiene lo que dejaron llamadas posteriores (direcciones de retorno, marcos
  // de C o x86, temporales). Esos bytes se enmascaran; el hash estricto los incluye.
  // Igual con la pila del scratchpad falso del PC (*fake-scratchpad-stack*: 64 KB, cima de la
  // pila en +32 KB; la parte baja son datos de trabajo y SÍ se comparan). Cuando se envía la
  // cadena no corre ningún proceso: las dos pilas están muertas.
  struct Range {
    u32 lo, hi;
  };
  Range stacks[2] = {{0, 0}, {0, 0}};
  if (auto ds = jak1::find_symbol_from_c("*dram-stack*"); ds.offset) {
    if (u32 arr = *Ptr<u32>(ds.offset).c()) {
      stacks[0] = {arr - 16, arr + 0x8000 + 16};
    }
  }
  if (auto fs = jak1::find_symbol_from_c("*fake-scratchpad-stack*"); fs.offset) {
    if (u32 base = *Ptr<u32>(fs.offset).c()) {
      stacks[1] = {base + 0x4000, base + 0x8000 + 16};
    }
  }
  u64 stack_masked = 0;
  DmaFollower dma(g_ee_main_mem, chain);
  u64 transfers = 0, bytes = 0;
  std::vector<u8> masked;
  while (!dma.ended() && transfers < 2000000) {
    auto t = dma.read_and_advance();
    mix((const u8*)&t.transferred_tag, 8);
    mix_strict((const u8*)&t.transferred_tag, 8);
    mix_strict(t.data, t.size_bytes);
    // Paquetes GIF (VIF DIRECT/DIRECTHL): se ponen a cero los campos que el GS ignora. El
    // código original deja ahí lo que haya en la pila (vectores 'stack-no-clear), y eso cambia
    // entre ejecuciones y entre x86 y C sin que cambie la imagen.
    u32 vif1_cmd = (u32)(t.transferred_tag >> 56) & 0x7f;
    u32 d_lo = t.data_offset, d_hi = t.data_offset + t.size_bytes;
    bool in_stack = false;
    for (auto& r : stacks) {
      in_stack |= t.size_bytes && r.hi && d_lo < r.hi && d_hi > r.lo;
    }
    if (in_stack) {
      masked.assign(t.data, t.data + t.size_bytes);
      for (auto& r : stacks) {
        if (r.hi && d_lo < r.hi && d_hi > r.lo) {
          u32 from = std::max(d_lo, r.lo) - d_lo, to = std::min(d_hi, r.hi) - d_lo;
          memset(masked.data() + from, 0, to - from);
          stack_masked += to - from;
        }
      }
      mix(masked.data(), t.size_bytes);
    } else if ((vif1_cmd == 0x50 || vif1_cmd == 0x51) && t.size_bytes) {
      masked.assign(t.data, t.data + t.size_bytes);
      mask_gif_ignored(masked.data(), t.size_bytes);
      mix(masked.data(), t.size_bytes);
    } else if ((vif1_cmd & 0x6f) == 0x6c && t.size_bytes) {
      // UNPACK V4-32 al VU1: misma basura en la w de vectores (S, T, Q=1.0, w)
      masked.assign(t.data, t.data + t.size_bytes);
      mask_unpack_stq_w(masked.data(), t.size_bytes);
      mix(masked.data(), t.size_bytes);
    } else {
      mix(t.data, t.size_bytes);
    }
    if (dump) {
      u32 hdr[4] = {(u32)transfers, t.data_offset, t.size_bytes, 0};
      fwrite(hdr, 4, 4, dump);
      fwrite(&t.transferred_tag, 8, 1, dump);
      fwrite(t.data, 1, t.size_bytes, dump);
    }
    transfers++;
    bytes += t.size_bytes;
  }
  if (dump) {
    fclose(dump);
    // GOAL2C_HUELLA_QUE=<dirección hex>: qué objetos hay justo antes de esa dirección
    if (const char* que = std::getenv("GOAL2C_HUELLA_QUE")) {
      u32 addr = (u32)std::strtoul(que, nullptr, 16);
      u32 type_type = *(s7 + FIX_SYM_TYPE_TYPE);
      int found = 0;
      for (u32 a = addr & ~3u; a > 0x100000 && a + 0x2000 > addr && found < 6; a -= 4) {
        u32 t = *Ptr<u32>(a).c();
        if (t > 0x100000 && t < 0x8000000 && (t & 7) == 4 && *Ptr<u32>(t - 4).c() == type_type) {
          u32 sym = *Ptr<u32>(t).c();  // type: symbol en +0
          lg::info("[huella] 0x{:x}: objeto de tipo {} con etiqueta en 0x{:x} (+0x{:x})", addr,
                   sym ? symbol_name(sym) : "?", a, addr - a);
          found++;
        }
      }
    }
  }
  lg::info("[huella] {} {:016x} estricto {:016x} ({} transferencias, {} bytes, {} en la pila)",
           g_fingerprint_frame++, h, hs, transfers, bytes, stack_masked);
}

#else  // !GOAL2C_MIXED_SUPPORTED

int replaced_functions(const char*) {
  return 0;
}

void frame_fingerprint(unsigned int) {}

void scripted_pad(unsigned long long) {}

void before_kernel_dispatch() {}

bool deterministic_mode() {
  return false;
}

bool virtual_ee_timer(unsigned long long*) {
  return false;
}

void after_link_jak1(const char*, ObjectFileHeader*) {
  static bool warned = false;
  const char* path = std::getenv("GOAL2C_LIB");
  if (path && *path && !warned) {
    warned = true;
    lg::warn("[goal2c] GOAL2C_LIB ignorada: el modo mixto solo existe en Linux x86-64");
  }
}

#endif

}  // namespace goal2c_runtime
