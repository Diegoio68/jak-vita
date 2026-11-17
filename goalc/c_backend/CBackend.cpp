/*!
 * @file CBackend.cpp
 * goal2c (Jak Vita): IR de goalc -> C. Ver CBackend.h.
 *
 * La semántica de cada instrucción replica la del backend x86 de goalc (IR.cpp), que es
 * la referencia con la que se valida (test/goal2c). Donde x86 tiene comportamiento que en C
 * sería indefinido (división por cero, float fuera de rango, desplazamientos >= 64), se usan
 * helpers de goal2c_rt.h.
 */
#include "CBackend.h"

#include <cstring>
#include <stdexcept>

#include "common/goal_constants.h"

#include "goalc/compiler/Env.h"
#include "goalc/compiler/IR.h"
#include "goalc/compiler/StaticObject.h"

#include "fmt/format.h"

namespace goal2c {

namespace {

class Unsupported : public std::runtime_error {
 public:
  explicit Unsupported(const std::string& what) : std::runtime_error(what) {}
};

[[noreturn]] void unsupported(const std::string& what) {
  throw Unsupported(what);
}

std::string escape_c_comment(std::string s) {
  // ni "*/" (cerraría el comentario) ni "/*" (gcc avisa de comentario anidado)
  for (const char* bad : {"*/", "/*"}) {
    size_t pos = 0;
    while ((pos = s.find(bad, pos)) != std::string::npos) {
      s.replace(pos, 2, std::string(1, bad[0]) + " " + bad[1]);
    }
  }
  return s;
}

std::string hex64(u64 v) {
  return fmt::format("0x{:x}ULL", v);
}

}  // namespace

std::string mangle(const std::string& goal_name) {
  std::string out = "g2c_";
  for (char c : goal_name) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out.push_back(c);
    } else if (c == '-') {
      out.push_back('_');
    } else if (c == '?') {
      out += "_P";
    } else if (c == '!') {
      out += "_X";
    } else {
      out += fmt::format("_{:02X}", (unsigned)(u8)c);
    }
  }
  return out;
}

///////////////////////////
// CFunctionWriter
///////////////////////////

CFunctionWriter::CFunctionWriter(const FunctionEnv* env, FileState* file)
    : m_env(env), m_file(file) {
  for (auto& c : const_cast<FunctionEnv*>(env)->constraints()) {
    if (c.contrain_everywhere) {
      m_everywhere[c.ireg.id] = &c;
    }
  }
}

void CFunctionWriter::check_constraints() const {
  // Los registros con significado propio (pp, rsp, r14, r15) solo se reconocen si la restricción
  // es "en toda la función" (rlet / self). Una restricción puntual a uno de ellos dejaría una
  // variable C normal (a cero) donde GOAL lee el registro: mejor no traducir la función.
  for (auto& c : const_cast<FunctionEnv*>(m_env)->constraints()) {
    auto r = c.desired_register;
    bool special = r == emitter::R13 || r == emitter::RSP || r == emitter::R14 || r == emitter::R15;
    if (special && !c.contrain_everywhere) {
      unsupported("restricción puntual a un registro especial (pp/rsp/r14/r15)");
    }
  }
}

bool CFunctionWriter::is_gpr(const RegVal* rv) const {
  return rv->ireg().reg_class == RegClass::GPR_64;
}

bool CFunctionWriter::is_pp(const RegVal* rv) const {
  if (rv->rlet_constraint().has_value()) {
    return *rv->rlet_constraint() == emitter::R13;
  }
  auto it = m_everywhere.find(rv->ireg().id);
  return it != m_everywhere.end() && it->second->desired_register == emitter::R13;
}

bool CFunctionWriter::is_float(const RegVal* rv) const {
  return rv->ireg().reg_class == RegClass::FLOAT;
}

std::string CFunctionWriter::ctype(const RegVal* rv) const {
  switch (rv->ireg().reg_class) {
    case RegClass::GPR_64:
      return "g2c_gpr";
    case RegClass::FLOAT:
      return "float";
    case RegClass::INT_128:
    case RegClass::VECTOR_FLOAT:
      return "g2c_q";
    default:
      unsupported("clase de registro inválida");
  }
}

std::string CFunctionWriter::var(const RegVal* rv) {
  std::optional<emitter::Register> constraint = rv->rlet_constraint();
  if (!constraint.has_value()) {
    auto it = m_everywhere.find(rv->ireg().id);
    if (it != m_everywhere.end()) {
      constraint = it->second->desired_register;
    }
  }
  if (constraint.has_value()) {
    // En C una variable rlet es una variable más: el registro concreto da igual. Salvo los
    // registros con significado propio en GOAL, cuyo valor no existe en el C.
    auto reg = *constraint;
    if (reg == emitter::R13) {
      // r13 = pp, el proceso actual (with-pp, y self en los behaviors). En C es la variable
      // global G2C_PP del runtime: leerla y escribirla es leer y escribir el registro.
      if (!is_gpr(rv)) {
        unsupported("pp en un registro que no es GPR");
      }
      return "G2C_PP";
    }
    // rsp, r14 (st) y r15 (base): GOAL solo los LEE (comprobar la pila en suspend, with-sp,
    // calcular direcciones). En C son expresiones de solo lectura: si algún código intentara
    // escribirlas, su C no compilaría y se vería enseguida.
    if (!is_gpr(rv)) {
      unsupported("rsp/r14/r15 en un registro que no es GPR");
    }
    if (reg == emitter::RSP) {
      return "G2C_SP_REG";  // marco de C actual: está en la misma pila que el proceso
    }
    if (reg == emitter::R15) {
      return "G2C_BASE_REG";  // base de la memoria GOAL
    }
    if (reg == emitter::R14) {
      return "((g2c_gpr)" + symbol("#f") + ")";  // tabla de símbolos: #f está en su inicio
    }
  }
  auto id = rv->ireg().id;
  if (rv->forced_on_stack()) {
    // (& var): en goalc vive siempre en un hueco de la pila y se lee/escribe ahí. En C es una
    // variable local cuya dirección escapa (ver finish), así que el compilador la mantiene en
    // memoria y la relee tras cualquier escritura a través de un puntero o llamada.
    m_addr_taken.insert(id);
  }
  auto t = ctype(rv);
  auto it = m_vars.find(id);
  if (it == m_vars.end()) {
    m_vars[id] = t;
  } else {
    ASSERT(it->second == t);
  }
  return var_prefix(t) + std::to_string(id);
}

std::string CFunctionWriter::var_prefix(const std::string& ctype) {
  if (ctype == "float") {
    return "f";
  }
  if (ctype == "g2c_q") {
    return "q";
  }
  return "r";
}

bool CFunctionWriter::is_q(const RegVal* rv) const {
  auto c = rv->ireg().reg_class;
  return c == RegClass::VECTOR_FLOAT || c == RegClass::INT_128;
}

std::string CFunctionWriter::label(int ir_idx) {
  m_label_targets.insert(ir_idx);
  return "L" + std::to_string(ir_idx);
}

std::string CFunctionWriter::symbol(const std::string& name) {
  auto it = m_file->symbols.find(name);
  int idx;
  if (it == m_file->symbols.end()) {
    idx = (int)m_file->symbols.size();
    m_file->symbols[name] = idx;
  } else {
    idx = it->second;
  }
  return fmt::format("g2c_sym[{}] /* {} */", idx, escape_c_comment(name));
}

std::string CFunctionWriter::static_addr(const StaticObject* obj) {
  auto it = m_file->static_idx.find(obj);
  if (it == m_file->static_idx.end()) {
    unsupported("objeto estático que no pertenece al fichero");
  }
  m_file->used_statics.insert(it->second);
  // el runtime da el comienzo del objeto; goalc apunta a start + get_addr_offset()
  // (p. ej. +4 en los basic, detrás de la etiqueta de tipo)
  auto off = obj->get_addr_offset();
  return fmt::format("(g2c_gpr)(g2c_static[{}] + {}u) /* {} */", it->second, off,
                     escape_c_comment(obj->print()));
}

std::string CFunctionWriter::function_addr(const FunctionEnv* fe) {
  auto it = m_file->func_addr_idx.find(fe);
  int idx;
  if (it == m_file->func_addr_idx.end()) {
    idx = (int)m_file->func_addr_idx.size();
    m_file->func_addr_idx[fe] = idx;
  } else {
    idx = it->second;
  }
  // como un estático: el runtime da su dirección en el objeto cargado (en la Vita el cargador
  // deja ahí el puntero C nativo; en el modo mixto es el código x86, parcheado si se traduce)
  return fmt::format("(g2c_gpr)g2c_fn[{}]", idx);
}

bool CFunctionWriter::writes_pp() const {
  for (auto& [ir, text] : m_body) {
    (void)ir;
    if (text.rfind("G2C_PP = ", 0) == 0) {
      return true;
    }
  }
  return false;
}

bool CFunctionWriter::has_q_signature() const {
  if (ret_is_q) {
    return true;
  }
  for (bool q : param_is_q) {
    if (q) {
      return true;
    }
  }
  return false;
}

std::string CFunctionWriter::stack_addr(int slot) {
  m_uses_stack = true;
  // goalc x86: rsp + 8 * slot - r15. Aquí el área de pila es un array local alineado a 16 (como
  // la pila de goalc), y la dirección GOAL es su dirección real menos la base.
  return fmt::format("(g2c_gpr)((uintptr_t)(g2c_stk + {}) - (uintptr_t)g2c_mem)", slot * 8);
}

void CFunctionWriter::line(const std::string& text) {
  m_body.emplace_back(current_ir, text);
}

void CFunctionWriter::begin_ir(int ir_idx) {
  current_ir = ir_idx;
  m_body.emplace_back(ir_idx, std::string());
}

void CFunctionWriter::reg_set(const RegVal* dest, const RegVal* src) {
  auto d = var(dest);
  auto s = var(src);
  if (is_gpr(dest) && is_gpr(src)) {
    line(fmt::format("{} = {};", d, s));
  } else if (is_float(dest) && is_float(src)) {
    line(fmt::format("{} = {};", d, s));
  } else if (is_float(dest) && is_gpr(src)) {
    // movd xmm, r32: toma los 32 bits bajos
    line(fmt::format("{} = g2c_bits_to_f((uint32_t){});", d, s));
  } else if (is_gpr(dest) && is_float(src)) {
    // movd r32, xmm + movsx: goalc extiende el signo (como mfc1 en la PS2)
    line(fmt::format("{} = (g2c_gpr)(g2c_sgpr)(int32_t)g2c_f_to_bits({});", d, s));
  } else if (is_q(dest) && is_q(src)) {
    line(fmt::format("{} = {};", d, s));
  } else if (is_float(dest) && is_q(src)) {
    // movss reg, reg: el elemento 0
    line(fmt::format("{} = {}.f[0];", d, s));
  } else if (is_q(dest) && is_float(src)) {
    // movss reg, reg: solo el elemento 0, el resto de dest se conserva
    line(fmt::format("{} = g2c_q_set_f0({}, {});", d, d, s));
  } else if (is_q(dest) && is_gpr(src)) {
    // movq xmm, r64: 64 bits bajos, la parte alta a cero
    line(fmt::format("{} = g2c_q_from_gpr({});", d, s));
  } else if (is_gpr(dest) && is_q(src)) {
    // movq r64, xmm: 64 bits bajos
    line(fmt::format("{} = (g2c_gpr){}.d[0];", d, s));
  } else {
    unsupported("reg_set entre clases no soportadas");
  }
}

std::string CFunctionWriter::finish(const std::string& c_name) {
  // Firma: los parámetros y el retorno de 128 bits (en goalc x86 van en xmm1..xmm8 y xmm0)
  // son g2c_q por valor; el resto, g2c_gpr. Un float como parámetro viaja en un GPR (como en
  // GOAL), así que un registro FLOAT como parámetro no debería aparecer.
  param_is_q.clear();
  for (auto* p : params) {
    if (is_q(p)) {
      param_is_q.push_back(true);
    } else if (is_gpr(p)) {
      param_is_q.push_back(false);
    } else {
      unsupported("parámetro que no es GPR ni de 128 bits");
    }
  }
  ret_is_q = return_reg && is_q(return_reg);
  if (return_reg && !ret_is_q && !is_gpr(return_reg)) {
    unsupported("retorno que no es GPR ni de 128 bits");
  }
  std::string out;
  // static: cada fichero solo exporta su tabla de funciones y su función de enlace
  out += fmt::format("static {} {}(", ret_is_q ? "g2c_q" : "g2c_gpr", c_name);
  for (size_t i = 0; i < params.size(); i++) {
    out += fmt::format("{}{} a{}", i ? ", " : "", param_is_q[i] ? "g2c_q" : "g2c_gpr", i);
  }
  if (params.empty()) {
    out += "void";
  }
  out += ") {\n";

  if (m_uses_stack) {
    int bytes = std::max(16, m_env->stack_slots_used_for_stack_vars() * 8);
    out += fmt::format("  uint8_t g2c_stk[{}] __attribute__((aligned(16)));\n", bytes);
    // El C solo usa la dirección de g2c_stk convertida a entero; sin esto el compilador puede
    // no reservarle sitio real en el marco y las llamadas lo machacan. El asm vacío hace que
    // la dirección escape: el array existe toda la función y cualquier llamada puede tocarlo.
    out += "  __asm__ volatile(\"\" : : \"r\"(g2c_stk) : \"memory\");\n";
  }
  // declaraciones (todas al principio: los goto no pueden saltarse inicializaciones)
  for (auto& [id, t] : m_vars) {
    out += fmt::format("  {} {}{} = {};\n", t, var_prefix(t), id, t == "g2c_q" ? "{{0}}" : "0");
  }
  for (int id : m_addr_taken) {
    // la dirección escapa: vive en memoria toda la función (ver var())
    out += fmt::format("  __asm__ volatile(\"\" : : \"r\"(&{}{}) : \"memory\");\n",
                       var_prefix(m_vars.at(id)), id);
  }
  for (size_t i = 0; i < params.size(); i++) {
    out += fmt::format("  {} = a{};\n", var(params[i]), i);
  }

  int last_ir = -1;
  for (auto& [ir_idx, text] : m_body) {
    if (ir_idx != last_ir) {
      // marcador de comienzo de instrucción: etiqueta (si es destino) y el IR como comentario
      if (m_label_targets.count(ir_idx)) {
        out += fmt::format("L{}:;\n", ir_idx);
      }
      out += fmt::format("  /* {} */\n", escape_c_comment(m_env->code().at(ir_idx)->print()));
      last_ir = ir_idx;
    }
    if (!text.empty()) {
      out += "  " + text + "\n";
    }
  }
  // destino = fin de la función (índice == número de instrucciones)
  int end_idx = (int)m_env->code().size();
  for (int target : m_label_targets) {
    if (target > end_idx) {
      unsupported(fmt::format("etiqueta fuera de la función ({})", target));
    }
  }
  if (m_label_targets.count(end_idx)) {
    out += fmt::format("L{}:;\n", end_idx);
  }
  if (return_reg) {
    out += fmt::format("  return {};\n", var(return_reg));
  } else {
    out += "  return 0;\n";
  }
  out += "}\n";
  return out;
}

///////////////////////////
// fichero completo
///////////////////////////

namespace {

std::string function_goal_name(FunctionEnv* fe) {
  if (fe->method_of_type_name != "#f" && fe->method_id.has_value()) {
    return fmt::format("(method {} {})", *fe->method_id, fe->method_of_type_name);
  }
  // deftype crea el inspect con este nombre y lo instala como método GOAL_INSPECT_METHOD
  const std::string auto_inspect = "autogenerated-inspect-method-of-";
  if (fe->name().rfind(auto_inspect, 0) == 0) {
    return fmt::format("(method {} {})", GOAL_INSPECT_METHOD,
                       fe->name().substr(auto_inspect.size()));
  }
  return fe->name();
}

struct TranslatedFunction {
  std::string text;
  int nargs = 0;
  std::vector<bool> param_is_q;
  bool ret_is_q = false;
  bool has_q = false;
  bool writes_pp = false;
};

TranslatedFunction translate_function(FunctionEnv* fe,
                                      const std::string& c_name,
                                      CFunctionWriter::FileState* file) {
  if (fe->is_asm_func) {
    unsupported("asm-func");
  }
  CFunctionWriter w(fe, file);
  w.check_constraints();
  auto& code = fe->code();
  for (size_t i = 0; i < code.size(); i++) {
    w.begin_ir((int)i);
    code[i]->do_codegen_c(w);
  }
  TranslatedFunction r;
  r.text = w.finish(c_name);
  r.nargs = (int)w.params.size();
  r.param_is_q = w.param_is_q;
  r.ret_is_q = w.ret_is_q;
  r.has_q = w.has_q_signature();
  r.writes_pp = w.writes_pp();
  return r;
}

/*!
 * Envoltorio con firma uniforme para las funciones con argumentos o retorno de 128 bits, para
 * el runtime de PC (x86 -> C): recibe los 8 GPR y los 8 xmm de argumentos de GOAL en arrays
 * y deja el retorno de 128 bits en *qr. En la Vita no se usa (la llamada es directa).
 */
std::string q_wrapper(const std::string& c_name, const TranslatedFunction& tf) {
  std::string call = c_name + "(";
  int gi = 0, qi = 0;
  for (size_t i = 0; i < tf.param_is_q.size(); i++) {
    call += i ? ", " : "";
    call += tf.param_is_q[i] ? fmt::format("q[{}]", qi++) : fmt::format("(g2c_gpr)g[{}]", gi++);
  }
  call += ")";
  std::string out = fmt::format(
      "static g2c_gpr {}__q(const uint64_t* g, const g2c_q* q, g2c_q* qr) {{\n"
      "  (void)g;\n  (void)q;\n  (void)qr;\n",
      c_name);
  if (tf.ret_is_q) {
    out += fmt::format("  *qr = {};\n  return 0;\n}}\n\n", call);
  } else {
    out += fmt::format("  return {};\n}}\n\n", call);
  }
  return out;
}

}  // namespace

FileResult emit_file(FileEnv* env,
                     const std::string& obj_name,
                     const StaticLocations& statics,
                     const FunctionLocations& func_locs,
                     const std::vector<u8>& object_data) {
  FileResult result;
  CFunctionWriter::FileState state;
  for (size_t i = 0; i < env->statics().size(); i++) {
    state.static_idx[env->statics()[i].get()] = (int)i;
  }
  std::string functions;
  struct TableEntry {
    std::string goal_name, c_name;
    int nargs;
    bool has_q;
    bool writes_pp;
    FunctionLocation loc;
  };
  std::vector<TableEntry> table;
  std::set<std::string> used_c_names;

  const FunctionEnv* top = &env->top_level_function();
  for (auto& fe_ptr : env->functions()) {
    FunctionEnv* fe = fe_ptr.get();
    if (fe == top) {
      continue;  // el código top-level del fichero se queda en el runtime por ahora
    }
    auto goal_name = function_goal_name(fe);
    auto c_name = mangle(goal_name);
    while (used_c_names.count(c_name)) {
      c_name += "_";
    }
    used_c_names.insert(c_name);

    auto state_backup = state;
    try {
      auto tf = translate_function(fe, c_name, &state);
      int nargs = tf.nargs;
      functions += fmt::format("/* {} */\n", escape_c_comment(goal_name));
      functions += tf.text + "\n";
      if (tf.has_q) {
        functions += q_wrapper(c_name, tf);
      } else if (nargs > 6) {
        // desde x86 solo hay trampolín directo para 6 argumentos; con más, el runtime de PC
        // pasa los 8 registros GOAL en un array
        functions += fmt::format("static g2c_gpr {}__arr(const uint64_t* a) {{\n  return {}(",
                                 c_name, c_name);
        for (int i = 0; i < nargs; i++) {
          functions += fmt::format("{}(g2c_gpr)a[{}]", i ? ", " : "", i);
        }
        functions += ");\n}\n\n";
      }
      FunctionLocation loc;
      auto lit = func_locs.find(fe);
      if (lit != func_locs.end()) {
        loc = lit->second;
      }
      table.push_back({goal_name, c_name, nargs, tf.has_q, tf.writes_pp, loc});
      result.translated.push_back(goal_name);
    } catch (Unsupported& e) {
      state = state_backup;
      result.skipped.push_back(fmt::format("{}: {}", goal_name, e.what()));
      functions += fmt::format("/* NO TRADUCIDA: {} -- {} */\n\n", escape_c_comment(goal_name),
                               escape_c_comment(e.what()));
    }
  }

  // tamaños de segmento del objeto generado. Cabecera v3 (generate_header_v3): "GOAL",
  // u16 major, u16 minor, u32 versión, u32 nº segmentos, link_seg[3]{offset,size} y
  // code_seg[3]{offset,size}: el size del segmento seg está en 40 + 8 * seg + 4.
  u32 seg_sizes[3] = {0, 0, 0};
  if (object_data.size() >= 64 && memcmp(object_data.data(), "GOAL", 4) == 0) {
    for (int seg = 0; seg < 3; seg++) {
      memcpy(&seg_sizes[seg], object_data.data() + 40 + 8 * seg + 4, 4);
    }
  } else {
    throw std::runtime_error("goal2c: objeto generado sin cabecera v3");
  }

  auto file_c = mangle(obj_name);
  std::string out;
  out +=
      fmt::format("/* Generado por goal2c (Jak Vita) desde {}. NO EDITAR A MANO. */\n", obj_name);
  out += "#include \"goal2c_rt.h\"\n\n";
  // el IR de goalc deja registros que se escriben y no se leen: no es un error aquí
  out += "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"\n";
  out += "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n\n";
  // tabla de símbolos: nombre -> dirección GOAL, rellenada al enlazar
  std::vector<std::string> sym_names(state.symbols.size());
  for (auto& [name, idx] : state.symbols) {
    sym_names.at(idx) = name;
  }
  out += fmt::format("static uint32_t g2c_sym[{}];\n", std::max<size_t>(1, sym_names.size()));
  // estáticos: índice = posición en FileEnv::statics()
  out +=
      fmt::format("static uint32_t g2c_static[{}];\n", std::max<size_t>(1, env->statics().size()));
  // objetos función del fichero cuya dirección se usa (lambdas): índice -> dirección GOAL
  out +=
      fmt::format("static uint32_t g2c_fn[{}];\n", std::max<size_t>(1, state.func_addr_idx.size()));
  out += fmt::format("void {}_link(void) {{\n", file_c);
  for (size_t i = 0; i < sym_names.size(); i++) {
    out += fmt::format("  g2c_sym[{}] = g2c_rt_symbol(\"{}\");\n", i, sym_names[i]);
  }
  for (int i : state.used_statics) {
    auto* obj = env->statics().at(i).get();
    auto it = statics.find(obj);
    if (it == statics.end() || it->second.second < 0) {
      throw std::runtime_error(
          fmt::format("goal2c: no se conoce la ubicación del estático {} de {}", i, obj_name));
    }
    out += fmt::format("  g2c_static[{}] = g2c_rt_static(\"{}\", {}, {});\n", i, obj_name,
                       it->second.first, it->second.second);
  }
  for (auto& [fe, idx] : state.func_addr_idx) {
    auto it = func_locs.find(fe);
    if (it == func_locs.end() || it->second.offset < 0) {
      throw std::runtime_error(fmt::format(
          "goal2c: no se conoce la ubicación de la función {} de {}", fe->name(), obj_name));
    }
    out += fmt::format("  g2c_fn[{}] = g2c_rt_static(\"{}\", {}, {}); /* {} */\n", idx, obj_name,
                       it->second.seg, it->second.offset, escape_c_comment(fe->name()));
  }
  out += "}\n\n";
  out += functions;
  out += fmt::format("const g2c_func_entry {}_funcs[] = {{\n", file_c);
  for (auto& e : table) {
    // {nombre, fn, nargs, fn_arr, fn_q, flags, seg, offset, length}
    out += fmt::format(
        "  {{\"{}\", (void*){}, {}, {}, {}, {}, {}, {}, {}}},\n", e.goal_name, e.c_name, e.nargs,
        (!e.has_q && e.nargs > 6) ? "(void*)" + e.c_name + "__arr" : std::string("0"),
        e.has_q ? "(void*)" + e.c_name + "__q" : std::string("0"),
        e.writes_pp ? "G2C_FN_WRITES_PP" : "0", e.loc.seg, e.loc.offset, e.loc.length);
  }
  out += "  {0, 0, 0, 0, 0, 0, 0, 0, 0}\n};\n\n";
  out +=
      fmt::format("const g2c_file_info {}_info = {{\"{}\", {}_link, {}_funcs, {{{}, {}, {}}}}};\n",
                  file_c, obj_name, file_c, file_c, seg_sizes[0], seg_sizes[1], seg_sizes[2]);
  result.c_source = out;
  return result;
}

}  // namespace goal2c

///////////////////////////
// IR -> C
///////////////////////////
using goal2c::CFunctionWriter;
using goal2c::unsupported;

void IR::do_codegen_c(CFunctionWriter& w) {
  (void)w;
  unsupported("instrucción IR no soportada: " + print());
}

void IR_ValueReset::do_codegen_c(CFunctionWriter& w) {
  // En la primera instrucción marca los parámetros (en orden). Después no genera código.
  if (w.current_ir == 0) {
    for (auto* rv : m_args) {
      // self de los behaviors va en la lista pero no es un argumento: llega en r13 (pp)
      if (w.is_pp(rv)) {
        continue;
      }
      w.params.push_back(rv);
    }
  }
}

void IR_Null::do_codegen_c(CFunctionWriter& w) {
  (void)w;
}

void IR_Nop::do_codegen_c(CFunctionWriter& w) {
  (void)w;
}

void IR_LoadConstant64::do_codegen_c(CFunctionWriter& w) {
  if (!w.is_gpr(m_dest)) {
    unsupported("load-constant a registro no GPR");
  }
  w.line(fmt::format("{} = (g2c_gpr){};", w.var(m_dest), goal2c::hex64(m_value)));
}

void IR_LoadSymbolPointer::do_codegen_c(CFunctionWriter& w) {
  auto sym = w.symbol(m_name);
  if (w.is_gpr(m_dest)) {
    w.line(fmt::format("{} = {};", w.var(m_dest), sym));
  } else if (w.is_float(m_dest)) {
    // goalc x86 (solo #f): movq xmm, s7 -> los 32 bits bajos como float
    w.line(fmt::format("{} = g2c_bits_to_f((uint32_t){});", w.var(m_dest), sym));
  } else if (w.is_q(m_dest)) {
    w.line(fmt::format("{} = g2c_q_from_gpr({});", w.var(m_dest), sym));
  } else {
    unsupported("puntero a símbolo en registro de clase desconocida");
  }
}

void IR_GetSymbolValue::do_codegen_c(CFunctionWriter& w) {
  if (!w.is_gpr(m_dest)) {
    unsupported("valor de símbolo a registro no GPR");
  }
  auto sym = w.symbol(m_src->name());
  if (m_sext) {
    w.line(fmt::format("{} = (g2c_gpr)(g2c_sgpr)g2c_ld_s32({});", w.var(m_dest), sym));
  } else {
    w.line(fmt::format("{} = (g2c_gpr)g2c_ld_u32({});", w.var(m_dest), sym));
  }
}

void IR_SetSymbolValue::do_codegen_c(CFunctionWriter& w) {
  if (!w.is_gpr(m_src)) {
    unsupported("set! de símbolo desde registro no GPR");
  }
  w.line(fmt::format("g2c_st_u32({}, (uint32_t){});", w.symbol(m_dest->name()), w.var(m_src)));
}

void IR_RegSet::do_codegen_c(CFunctionWriter& w) {
  w.reg_set(m_dest, m_src);
}

void IR_Return::do_codegen_c(CFunctionWriter& w) {
  if (w.return_reg && w.return_reg->ireg().id != m_return_reg->ireg().id) {
    unsupported("varios registros de retorno distintos");
  }
  w.return_reg = m_return_reg;
  if (m_value->ireg().id != m_return_reg->ireg().id) {
    w.reg_set(m_return_reg, m_value);
  }
}

void IR_GotoLabel::do_codegen_c(CFunctionWriter& w) {
  ASSERT(m_resolved);
  w.line(fmt::format("goto {};", w.label(m_dest->idx)));
}

void IR_ConditionalBranch::do_codegen_c(CFunctionWriter& w) {
  ASSERT(m_resolved);
  auto a = w.var(condition.a);
  auto b = w.var(condition.b);
  std::string cond;
  if (condition.is_float) {
    if (condition.is_signed) {
      unsupported("comparación de float con signo (no la genera goalc)");
    }
    const char* macro = nullptr;
    switch (condition.kind) {
      case ConditionKind::EQUAL:
        macro = "G2C_FEQ";
        break;
      case ConditionKind::NOT_EQUAL:
        macro = "G2C_FNE";
        break;
      case ConditionKind::LT:
        macro = "G2C_FLT";
        break;
      case ConditionKind::LEQ:
        macro = "G2C_FLE";
        break;
      case ConditionKind::GT:
        macro = "G2C_FGT";
        break;
      case ConditionKind::GEQ:
        macro = "G2C_FGE";
        break;
      default:
        unsupported("condición inválida");
    }
    cond = fmt::format("{}({}, {})", macro, a, b);
  } else {
    const char* op = nullptr;
    switch (condition.kind) {
      case ConditionKind::EQUAL:
        op = "==";
        break;
      case ConditionKind::NOT_EQUAL:
        op = "!=";
        break;
      case ConditionKind::LT:
        op = "<";
        break;
      case ConditionKind::LEQ:
        op = "<=";
        break;
      case ConditionKind::GT:
        op = ">";
        break;
      case ConditionKind::GEQ:
        op = ">=";
        break;
      default:
        unsupported("condición inválida");
    }
    if (condition.is_signed) {
      cond = fmt::format("(g2c_sgpr){} {} (g2c_sgpr){}", a, op, b);
    } else {
      cond = fmt::format("{} {} {}", a, op, b);
    }
  }
  w.line(fmt::format("if ({}) goto {};", cond, w.label(label.idx)));
}

void IR_IntegerMath::do_codegen_c(CFunctionWriter& w) {
  auto d = w.var(m_dest);
  auto arg = [&]() { return w.var(m_arg); };
  switch (m_kind) {
    case IntegerMathKind::ADD_64:
      w.line(fmt::format("{} = {} + {};", d, d, arg()));
      break;
    case IntegerMathKind::SUB_64:
      w.line(fmt::format("{} = {} - {};", d, d, arg()));
      break;
    case IntegerMathKind::AND_64:
      w.line(fmt::format("{} = {} & {};", d, d, arg()));
      break;
    case IntegerMathKind::OR_64:
      w.line(fmt::format("{} = {} | {};", d, d, arg()));
      break;
    case IntegerMathKind::XOR_64:
      w.line(fmt::format("{} = {} ^ {};", d, d, arg()));
      break;
    case IntegerMathKind::NOT_64:
      w.line(fmt::format("{} = ~{};", d, d));
      break;
    case IntegerMathKind::SHL_64:
      w.line(fmt::format("{} = {} << ({} & G2C_SHIFT_MASK);", d, d, (int)m_shift_amount));
      break;
    case IntegerMathKind::SHR_64:
      w.line(fmt::format("{} = {} >> ({} & G2C_SHIFT_MASK);", d, d, (int)m_shift_amount));
      break;
    case IntegerMathKind::SAR_64:
      w.line(fmt::format("{} = (g2c_gpr)((g2c_sgpr){} >> ({} & G2C_SHIFT_MASK));", d, d,
                         (int)m_shift_amount));
      break;
    // x86 enmascara la cuenta de desplazamiento a 6 bits
    case IntegerMathKind::SHLV_64:
      w.line(fmt::format("{} = {} << ({} & G2C_SHIFT_MASK);", d, d, arg()));
      break;
    case IntegerMathKind::SHRV_64:
      w.line(fmt::format("{} = {} >> ({} & G2C_SHIFT_MASK);", d, d, arg()));
      break;
    case IntegerMathKind::SARV_64:
      w.line(fmt::format("{} = (g2c_gpr)((g2c_sgpr){} >> ({} & G2C_SHIFT_MASK));", d, d, arg()));
      break;
    case IntegerMathKind::IMUL_32:
      // multiplicación de 32 bits; la PS2 extiende el signo del resultado
      w.line(fmt::format("{} = (g2c_gpr)(g2c_sgpr)(int32_t)((uint32_t){} * (uint32_t){});", d, d,
                         arg()));
      break;
    case IntegerMathKind::IMUL_64:
      w.line(fmt::format("{} = {} * {};", d, d, arg()));
      break;
    case IntegerMathKind::IDIV_32:
      w.line(fmt::format("{} = (g2c_gpr)(g2c_sgpr)g2c_idiv32((int32_t){}, (int32_t){});", d, d,
                         arg()));
      break;
    case IntegerMathKind::IMOD_32:
      w.line(fmt::format("{} = (g2c_gpr)(g2c_sgpr)g2c_imod32((int32_t){}, (int32_t){});", d, d,
                         arg()));
      break;
    // goalc extiende el signo también en la división sin signo (como PCSX2)
    case IntegerMathKind::UDIV_32:
      w.line(fmt::format("{} = (g2c_gpr)(g2c_sgpr)(int32_t)g2c_udiv32((uint32_t){}, (uint32_t){});",
                         d, d, arg()));
      break;
    case IntegerMathKind::UMOD_32:
      w.line(fmt::format("{} = (g2c_gpr)(g2c_sgpr)(int32_t)g2c_umod32((uint32_t){}, (uint32_t){});",
                         d, d, arg()));
      break;
    default:
      unsupported("operación entera desconocida");
  }
}

void IR_FloatMath::do_codegen_c(CFunctionWriter& w) {
  auto d = w.var(m_dest);
  auto a = w.var(m_arg);
  switch (m_kind) {
    case FloatMathKind::ADD_SS:
      w.line(fmt::format("{} = {} + {};", d, d, a));
      break;
    case FloatMathKind::SUB_SS:
      w.line(fmt::format("{} = {} - {};", d, d, a));
      break;
    case FloatMathKind::MUL_SS:
      w.line(fmt::format("{} = {} * {};", d, d, a));
      break;
    case FloatMathKind::DIV_SS:
      w.line(fmt::format("{} = {} / {};", d, d, a));
      break;
    // minss/maxss devuelven el segundo operando si hay NaN o son iguales
    case FloatMathKind::MIN_SS:
      w.line(fmt::format("{} = ({} < {}) ? {} : {};", d, d, a, d, a));
      break;
    case FloatMathKind::MAX_SS:
      w.line(fmt::format("{} = ({} > {}) ? {} : {};", d, d, a, d, a));
      break;
    case FloatMathKind::SQRT_SS:
      w.line(fmt::format("{} = __builtin_sqrtf({});", d, a));
      break;
    default:
      unsupported("operación float desconocida");
  }
}

void IR_FloatToInt::do_codegen_c(CFunctionWriter& w) {
  if (!w.is_gpr(m_dest) || !w.is_float(m_src)) {
    unsupported("float->int con clases inesperadas");
  }
  w.line(fmt::format("{} = (g2c_gpr)(g2c_sgpr)g2c_f2i({});", w.var(m_dest), w.var(m_src)));
}

void IR_IntToFloat::do_codegen_c(CFunctionWriter& w) {
  if (!w.is_float(m_dest) || !w.is_gpr(m_src)) {
    unsupported("int->float con clases inesperadas");
  }
  // cvtsi2ss con origen de 32 bits
  w.line(fmt::format("{} = (float)(int32_t){};", w.var(m_dest), w.var(m_src)));
}

void IR_StaticVarLoad::do_codegen_c(CFunctionWriter& w) {
  if (w.is_q(m_dest)) {
    // goalc x86: loadvf_rip enlazado al comienzo del estático (16 bytes)
    w.line(fmt::format("{} = g2c_ld_q({});", w.var(m_dest), w.static_addr(m_src)));
    return;
  }
  auto* sf = dynamic_cast<const StaticFloat*>(m_src);
  if (!sf || !w.is_float(m_dest)) {
    unsupported("carga estática que no es un float");
  }
  u32 bits;
  memcpy(&bits, &sf->value, 4);
  w.line(fmt::format("{} = g2c_bits_to_f(0x{:08x}u); /* {} */", w.var(m_dest), bits, sf->value));
}

void IR_LoadConstOffset::do_codegen_c(CFunctionWriter& w) {
  auto d = w.var(m_dest);
  auto addr = fmt::format("{} + {}", w.var(m_base), m_offset);
  if (w.is_gpr(m_dest)) {
    const char* fn = nullptr;
    switch (m_info.size) {
      case 1:
        fn = m_info.sign_extend ? "g2c_ld_s8" : "g2c_ld_u8";
        break;
      case 2:
        fn = m_info.sign_extend ? "g2c_ld_s16" : "g2c_ld_u16";
        break;
      case 4:
        fn = m_info.sign_extend ? "g2c_ld_s32" : "g2c_ld_u32";
        break;
      case 8:
        fn = "g2c_ld_u64";
        break;
      default:
        unsupported("load de tamaño no soportado");
    }
    if (m_info.sign_extend) {
      w.line(fmt::format("{} = (g2c_gpr)(g2c_sgpr){}({});", d, fn, addr));
    } else {
      w.line(fmt::format("{} = (g2c_gpr){}({});", d, fn, addr));
    }
  } else if (w.is_float(m_dest) && m_info.size == 4 && !m_info.sign_extend &&
             m_info.reg == RegClass::FLOAT) {
    w.line(fmt::format("{} = g2c_ld_f32({});", d, addr));
  } else if (w.is_q(m_dest) && m_info.size == 16 && !m_info.sign_extend) {
    w.line(fmt::format("{} = g2c_ld_q({});", d, addr));
  } else {
    unsupported("load a registro no soportado");
  }
}

void IR_StoreConstOffset::do_codegen_c(CFunctionWriter& w) {
  auto v = w.var(m_value);
  auto addr = fmt::format("{} + {}", w.var(m_base), m_offset);
  if (w.is_gpr(m_value)) {
    switch (m_size) {
      case 1:
        w.line(fmt::format("g2c_st_u8({}, (uint8_t){});", addr, v));
        break;
      case 2:
        w.line(fmt::format("g2c_st_u16({}, (uint16_t){});", addr, v));
        break;
      case 4:
        w.line(fmt::format("g2c_st_u32({}, (uint32_t){});", addr, v));
        break;
      case 8:
        w.line(fmt::format("g2c_st_u64({}, (uint64_t){});", addr, v));
        break;
      default:
        unsupported("store de tamaño no soportado");
    }
  } else if (w.is_float(m_value) && m_size == 4) {
    w.line(fmt::format("g2c_st_f32({}, {});", addr, v));
  } else if (w.is_q(m_value) && m_size == 16) {
    w.line(fmt::format("g2c_st_q({}, {});", addr, v));
  } else {
    unsupported("store desde registro no soportado");
  }
}

void IR_StaticVarAddr::do_codegen_c(CFunctionWriter& w) {
  if (!w.is_gpr(m_dest)) {
    unsupported("dirección estática a registro no GPR");
  }
  w.line(fmt::format("{} = {};", w.var(m_dest), w.static_addr(m_src)));
}

void IR_FunctionCall::do_codegen_c(CFunctionWriter& w) {
  // goalc x86: add freg, r15; call freg. En goal2c se usa G2C_CALLn (goal2c_rt.h): en la
  // Vita lee el puntero C nativo del objeto función (G2C_NATIVE_FN) y llama con la aridad
  // del llamante, como en GOAL (los argumentos sobrantes se ignoran; con AAPCS y SysV el
  // llamante limpia la pila). En el modo mixto de PC pasa por el runtime.
  if (!w.is_gpr(m_func)) {
    unsupported("llamada con la función en un registro no GPR");
  }
  if (m_args.size() > 8) {
    unsupported("llamada con más de 8 argumentos");
  }
  bool any_q = w.is_q(m_ret);
  for (auto* a : m_args) {
    if (w.is_q(a)) {
      any_q = true;
    } else if (!w.is_gpr(a)) {
      unsupported("llamada con argumento que no es GPR ni de 128 bits");
    }
  }
  if (!any_q) {
    if (!w.is_gpr(m_ret)) {
      unsupported("llamada con retorno que no es GPR ni de 128 bits");
    }
    std::string args = w.var(m_func);
    for (auto* a : m_args) {
      args += ", " + w.var(a);
    }
    w.line(fmt::format("{} = G2C_CALL{}({});", w.var(m_ret), m_args.size(), args));
    return;
  }

  // Con argumentos o retorno de 128 bits (goalc x86: xmm1..xmm8 por orden de aparición entre
  // los de 128 bits, retorno en xmm0). En la Vita: llamada directa con la firma exacta, como
  // la que tiene la función C llamada (los g2c_q van por valor). En el modo mixto: el runtime
  // carga los xmm (g2c_host_call_q).
  bool ret_q = w.is_q(m_ret);
  if (!ret_q && !w.is_gpr(m_ret)) {
    unsupported("llamada con retorno que no es GPR ni de 128 bits");
  }
  std::string sig, args, gprs, qs;
  for (size_t i = 0; i < m_args.size(); i++) {
    auto* a = m_args[i];
    bool q = w.is_q(a);
    sig += fmt::format("{}{}", i ? ", " : "", q ? "g2c_q" : "g2c_gpr");
    args += fmt::format("{}{}", i ? ", " : "", w.var(a));
    auto& list = q ? qs : gprs;
    list += fmt::format("{}{}", list.empty() ? "" : ", ", w.var(a));
  }
  if (sig.empty()) {
    sig = "void";
  }
  if (gprs.empty()) {
    gprs = "0";
  }
  // sin argumentos de 128 bits: puntero nulo (el runtime pone los xmm a cero)
  std::string qarr = qs.empty() ? "0" : fmt::format("(const g2c_q[8]){{{}}}", qs);
  auto ret = w.var(m_ret);
  w.line("#ifdef G2C_PC_MIXED");
  if (ret_q) {
    w.line(fmt::format(
        "{{ g2c_q g2c_qr; (void)g2c_host_call_q((uint32_t){}, (const uint64_t[8]){{{}}}, {}, "
        "&g2c_qr); {} = g2c_qr; }}",
        w.var(m_func), gprs, qarr, ret));
  } else {
    w.line(fmt::format(
        "{{ g2c_q g2c_qr; {} = (g2c_gpr)g2c_host_call_q((uint32_t){}, (const uint64_t[8]){{{}}}, "
        "{}, &g2c_qr); }}",
        ret, w.var(m_func), gprs, qarr));
  }
  w.line("#else");
  w.line(fmt::format("{} = (({}(*)({}))G2C_NATIVE_FN({}))({});", ret, ret_q ? "g2c_q" : "g2c_gpr",
                     sig, w.var(m_func), args));
  w.line("#endif");
}

void IR_FunctionAddr::do_codegen_c(CFunctionWriter& w) {
  // goalc x86: lea rip-relativo al objeto función (otra función del mismo fichero) - r15
  if (!w.is_gpr(m_dest)) {
    unsupported("dirección de función a registro no GPR");
  }
  w.line(fmt::format("{} = {};", w.var(m_dest), w.function_addr(m_src)));
}

void IR_RegValAddr::do_codegen_c(CFunctionWriter& w) {
  // goalc x86: lea dst, [rsp + hueco de la variable]; sub dst, r15. Aquí la variable es una
  // local de C con la dirección expuesta (ver CFunctionWriter::var).
  if (!w.is_gpr(m_dest)) {
    unsupported("dirección de variable a registro no GPR");
  }
  auto src = w.var(m_src);
  w.line(fmt::format("{} = (g2c_gpr)((uintptr_t)&{} - (uintptr_t)g2c_mem);", w.var(m_dest), src));
}

///////////////////////////
// vectores y enteros de 128 bits (ensamblador en línea de GOAL: .add.vf, .pextlw, ...)
///////////////////////////

void IR_RegSetAsm::do_codegen_c(CFunctionWriter& w) {
  w.reg_set(m_dst, m_src);
}

void IR_AsmFNop::do_codegen_c(CFunctionWriter& w) {
  (void)w;
}

void IR_AsmFWait::do_codegen_c(CFunctionWriter& w) {
  (void)w;
}

void IR_GetSymbolValueAsm::do_codegen_c(CFunctionWriter& w) {
  if (!w.is_gpr(m_dest)) {
    unsupported("valor de símbolo (asm) a registro no GPR");
  }
  auto sym = w.symbol(m_sym_name);
  if (m_sext) {
    w.line(fmt::format("{} = (g2c_gpr)(g2c_sgpr)g2c_ld_s32({});", w.var(m_dest), sym));
  } else {
    w.line(fmt::format("{} = (g2c_gpr)g2c_ld_u32({});", w.var(m_dest), sym));
  }
}

namespace {
void need_q(CFunctionWriter& w, const RegVal* r, const char* what) {
  if (!w.is_q(r)) {
    unsupported(std::string(what) + ": operando que no es de 128 bits");
  }
}
}  // namespace

void IR_VFMath3Asm::do_codegen_c(CFunctionWriter& w) {
  need_q(w, m_dst, "vf3");
  need_q(w, m_src1, "vf3");
  need_q(w, m_src2, "vf3");
  const char* fn = nullptr;
  switch (m_kind) {
    case Kind::XOR:
      fn = "g2c_q_xor";
      break;
    case Kind::SUB:
      fn = "g2c_qf_sub";
      break;
    case Kind::ADD:
      fn = "g2c_qf_add";
      break;
    case Kind::MUL:
      fn = "g2c_qf_mul";
      break;
    case Kind::MAX:
      fn = "g2c_qf_max";
      break;
    case Kind::MIN:
      fn = "g2c_qf_min";
      break;
    case Kind::DIV:
      fn = "g2c_qf_div";
      break;
    default:
      unsupported("operación vf desconocida");
  }
  w.line(fmt::format("{} = {}({}, {});", w.var(m_dst), fn, w.var(m_src1), w.var(m_src2)));
}

void IR_VFMath2Asm::do_codegen_c(CFunctionWriter& w) {
  need_q(w, m_dst, "vf2");
  need_q(w, m_src, "vf2");
  const char* fn = m_kind == Kind::ITOF ? "g2c_qf_itof" : "g2c_qf_ftoi";
  w.line(fmt::format("{} = {}({});", w.var(m_dst), fn, w.var(m_src)));
}

void IR_BlendVF::do_codegen_c(CFunctionWriter& w) {
  need_q(w, m_dst, "blend");
  need_q(w, m_src1, "blend");
  need_q(w, m_src2, "blend");
  w.line(fmt::format("{} = g2c_q_blend({}, {}, 0x{:x}u);", w.var(m_dst), w.var(m_src1),
                     w.var(m_src2), (unsigned)m_mask));
}

void IR_SplatVF::do_codegen_c(CFunctionWriter& w) {
  need_q(w, m_dst, "splat");
  need_q(w, m_src, "splat");
  unsigned ctrl;
  switch (m_element) {
    case emitter::Register::VF_ELEMENT::X:
      ctrl = 0b00000000;
      break;
    case emitter::Register::VF_ELEMENT::Y:
      ctrl = 0b01010101;
      break;
    case emitter::Register::VF_ELEMENT::Z:
      ctrl = 0b10101010;
      break;
    case emitter::Register::VF_ELEMENT::W:
      ctrl = 0b11111111;
      break;
    default:
      unsupported("splat sin elemento");
  }
  w.line(fmt::format("{} = g2c_q_swizzle({}, 0x{:x}u);", w.var(m_dst), w.var(m_src), ctrl));
}

void IR_SwizzleVF::do_codegen_c(CFunctionWriter& w) {
  need_q(w, m_dst, "swizzle");
  need_q(w, m_src, "swizzle");
  w.line(fmt::format("{} = g2c_q_swizzle({}, 0x{:x}u);", w.var(m_dst), w.var(m_src),
                     (unsigned)m_controlBytes));
}

void IR_SqrtVF::do_codegen_c(CFunctionWriter& w) {
  need_q(w, m_dst, "sqrt");
  need_q(w, m_src, "sqrt");
  w.line(fmt::format("{} = g2c_qf_sqrt({});", w.var(m_dst), w.var(m_src)));
}

void IR_Int128Math3Asm::do_codegen_c(CFunctionWriter& w) {
  need_q(w, m_dst, "int128");
  need_q(w, m_src1, "int128");
  need_q(w, m_src2, "int128");
  const char* fn = nullptr;
  switch (m_kind) {
    case Kind::PEXTUB:
      fn = "g2c_pextub";
      break;
    case Kind::PEXTUH:
      fn = "g2c_pextuh";
      break;
    case Kind::PEXTUW:
      fn = "g2c_pextuw";
      break;
    case Kind::PEXTLB:
      fn = "g2c_pextlb";
      break;
    case Kind::PEXTLH:
      fn = "g2c_pextlh";
      break;
    case Kind::PEXTLW:
      fn = "g2c_pextlw";
      break;
    case Kind::PCPYUD:
      fn = "g2c_pcpyud";
      break;
    case Kind::PCPYLD:
      fn = "g2c_pcpyld";
      break;
    case Kind::PSUBW:
      fn = "g2c_psubw";
      break;
    case Kind::PCEQB:
      fn = "g2c_pceqb";
      break;
    case Kind::PCEQH:
      fn = "g2c_pceqh";
      break;
    case Kind::PCEQW:
      fn = "g2c_pceqw";
      break;
    case Kind::PCGTB:
      fn = "g2c_pcgtb";
      break;
    case Kind::PCGTH:
      fn = "g2c_pcgth";
      break;
    case Kind::PCGTW:
      fn = "g2c_pcgtw";
      break;
    case Kind::POR:
      fn = "g2c_q_or";
      break;
    case Kind::PXOR:
      fn = "g2c_q_xor";
      break;
    case Kind::PAND:
      fn = "g2c_q_and";
      break;
    case Kind::PACKUSWB:
      fn = "g2c_packuswb";
      break;
    case Kind::PADDB:
      fn = "g2c_paddb";
      break;
    default:
      unsupported("operación int128 desconocida");
  }
  w.line(fmt::format("{} = {}({}, {});", w.var(m_dst), fn, w.var(m_src1), w.var(m_src2)));
}

void IR_Int128Math2Asm::do_codegen_c(CFunctionWriter& w) {
  need_q(w, m_dst, "int128");
  need_q(w, m_src, "int128");
  if (!m_imm.has_value()) {
    unsupported("int128 sin inmediato");
  }
  const char* fn = nullptr;
  switch (m_kind) {
    case Kind::PW_SLL:
      fn = "g2c_pw_sll";
      break;
    case Kind::PW_SRL:
      fn = "g2c_pw_srl";
      break;
    case Kind::PH_SLL:
      fn = "g2c_ph_sll";
      break;
    case Kind::PH_SRL:
      fn = "g2c_ph_srl";
      break;
    case Kind::PW_SRA:
      fn = "g2c_pw_sra";
      break;
    case Kind::VPSRLDQ:
      fn = "g2c_psrldq";
      break;
    case Kind::VPSLLDQ:
      fn = "g2c_pslldq";
      break;
    case Kind::VPSHUFLW:
      fn = "g2c_pshuflw";
      break;
    case Kind::VPSHUFHW:
      fn = "g2c_pshufhw";
      break;
    default:
      unsupported("operación int128 desconocida");
  }
  w.line(fmt::format("{} = {}({}, {}u);", w.var(m_dst), fn, w.var(m_src), (unsigned)*m_imm));
}

void IR_GetStackAddr::do_codegen_c(CFunctionWriter& w) {
  if (!w.is_gpr(m_dest)) {
    unsupported("dirección de pila a registro no GPR");
  }
  w.line(fmt::format("{} = {};", w.var(m_dest), w.stack_addr(m_slot)));
}
