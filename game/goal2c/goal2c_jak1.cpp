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
 * - Las que escriben pp (G2C_FN_WRITES_PP: suspend, with-proc) se quedan en x86: suspend
 *   copia la pila del proceso a un área de 256 bytes y los marcos de C no caben.
 */
#include "goal2c_jak1.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "common/common_types.h"
#include "common/log/log.h"
#include "common/symbols.h"

#include "game/kernel/common/codegen.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"
#include "goalc/c_backend/runtime/goal2c_rt.h"

#if defined(__linux__) && defined(__x86_64__)
#define GOAL2C_MIXED_SUPPORTED 1
#include <csignal>
#include <cstdio>
#include <dlfcn.h>
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
  ret

  .globl g2c_call_goal_q
g2c_call_goal_q:
  push r13
  push r14
  push r15
  push rbx
  sub rsp, 8
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
  call rax
  movaps [rbx + 192], xmm0
  add rsp, 8
  pop rbx
  pop r15
  pop r14
  pop r13
  ret
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
std::map<std::string, int> g_replaced;  // objeto -> funciones sustituidas en su último enlace
ObjectFileHeader* g_cur_ofh = nullptr;
std::string g_cur_obj;

// r13 (pp) de la última entrada x86 -> C. Se guarda/restaura en cada llamada C -> x86.
u64 g_pp = 0;

u32 host_symbol(const char* name) {
  return jak1::intern_from_c(name).offset;
}

u32 host_static(const char* obj, int seg, int off) {
  if (!g_cur_ofh || g_cur_obj != obj || seg < 0 || seg >= N_SEG) {
    lg::error("[goal2c] estático pedido fuera del enlace de su objeto: {} seg {} +{}", obj, seg,
              off);
    return 0;
  }
  return g_cur_ofh->code_infos[seg].offset + (u32)off;
}

u64 host_call(u32 fn, int nargs, const u64* args) {
  u64 a8[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < nargs && i < 8; i++) {
    a8[i] = args[i];
  }
  u64 saved_pp = g_pp;
  u64 ret = _call_goal8_asm_systemv(g_ee_main_mem + fn, a8, 0, saved_pp, s7.offset, g_ee_main_mem);
  g_pp = saved_pp;
  return ret;
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
  auto mem = Ptr<u8>(jak1::alloc_heap_object(s7.offset + FIX_SYM_GLOBAL_HEAP,
                                             *(s7 + FIX_SYM_FUNCTION_TYPE), 0x40, UNKNOWN_PP));
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
                  : kind == Entry::STACK ? (u64)&_stack_call_systemv
                                         : (u64)&_arg_call_systemv);
  code[off++] = 0xff;
  code[off++] = 0xe0;
  ASSERT(off <= 0x40);
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
  for (int depth = 0; depth < 8 && fp; depth++) {
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
  // devolver el control al manejador anterior (o al de por defecto)
  sigaction(SIGSEGV, &g_prev_segv, nullptr);
  raise(sig);
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

}  // namespace

void after_link_jak1(const char* object_name, ObjectFileHeader* ofh) {
  update_library();
  if (g_files.empty()) {
    return;
  }
  std::string obj = object_name;
  auto slash = obj.find_last_of("/\\");
  if (slash != std::string::npos) {
    obj = obj.substr(slash + 1);
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
    if (e->flags & G2C_FN_WRITES_PP) {
      kept_pp++;  // suspend / with-proc: se queda en x86 (ver comentario del fichero)
      continue;
    }
    Entry kind = e->fn_q ? Entry::Q : (e->fn_arr ? Entry::STACK : Entry::ARGS);
    void* fn = e->fn_q ? e->fn_q : (e->fn_arr ? e->fn_arr : e->fn);
    std::string name = e->name;
    int method_id;
    std::string type_name;
    bool installed = false;
    if (parse_method(name, &method_id, &type_name)) {
      u32 type = *Ptr<u32>(jak1::intern_from_c(type_name.c_str()).offset).c();
      if (type) {
        jak1::method_set(type, method_id, make_c_entry(fn, kind));
        installed = true;
      } else {
        lg::warn("[goal2c] {}: tipo {} sin definir, no se sustituye {}", obj, type_name, name);
      }
    } else if (name.rfind("anon-function", 0) != 0) {
      u32* value = Ptr<u32>(jak1::intern_from_c(name.c_str()).offset).c();
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

#else  // !GOAL2C_MIXED_SUPPORTED

int replaced_functions(const char*) {
  return 0;
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
