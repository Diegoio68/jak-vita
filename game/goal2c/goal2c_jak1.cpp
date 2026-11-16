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
 * - Datos estáticos: el C pide (segmento, posición); la base del segmento sale de la
 *   cabecera del objeto recién enlazado.
 */
#include "goal2c_jak1.h"

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
}
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

void emit_movabs_rax(u8* code, int* off, u64 imm) {
  code[(*off)++] = 0x48;  // movabs rax, imm64
  code[(*off)++] = 0xb8;
  memcpy(code + *off, &imm, 8);
  *off += 8;
}

//! Objeto función GOAL que entra en la función C fn (ver comentario del fichero).
u32 make_c_entry(void* fn, bool stack_args) {
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
  emit_movabs_rax(code, &off, stack_args ? (u64)&_stack_call_systemv : (u64)&_arg_call_systemv);
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
  int replaced = 0, skipped = 0;
  for (auto* e = info->funcs; e->name; e++) {
    bool stack_args = e->nargs > 6;
    void* fn = stack_args ? e->fn_arr : e->fn;
    std::string name = e->name;
    int method_id;
    std::string type_name;
    if (parse_method(name, &method_id, &type_name)) {
      u32 type = *Ptr<u32>(jak1::intern_from_c(type_name.c_str()).offset).c();
      if (!type) {
        lg::warn("[goal2c] {}: tipo {} sin definir, no se sustituye {}", obj, type_name, name);
        skipped++;
        continue;
      }
      jak1::method_set(type, method_id, make_c_entry(fn, stack_args));
      replaced++;
    } else {
      u32* value = Ptr<u32>(jak1::intern_from_c(name.c_str()).offset).c();
      // solo si el top-level dejó ahí una función (anónimas, inline, etc. no se tocan)
      if (*value == 0 || *Ptr<u32>(*value - 4).c() != function_type) {
        lg::debug("[goal2c] {}: {} no tiene una función en su símbolo, se queda como está", obj,
                  name);
        skipped++;
        continue;
      }
      *value = make_c_entry(fn, stack_args);
      replaced++;
    }
  }
  g_replaced[obj] = replaced;
  lg::info("[goal2c] {}: {} funciones en C, {} se quedan en x86", obj, replaced, skipped);
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
