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
  size_t pos = 0;
  while ((pos = s.find("*/", pos)) != std::string::npos) {
    s.replace(pos, 2, "* /");
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

CFunctionWriter::CFunctionWriter(const FunctionEnv* env, std::map<std::string, int>* file_symbols)
    : m_env(env), m_file_symbols(file_symbols) {}

bool CFunctionWriter::is_gpr(const RegVal* rv) const {
  return rv->ireg().reg_class == RegClass::GPR_64;
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
      unsupported("registro INT_128 (todavía no soportado)");
    case RegClass::VECTOR_FLOAT:
      unsupported("registro VECTOR_FLOAT (todavía no soportado)");
    default:
      unsupported("clase de registro inválida");
  }
}

std::string CFunctionWriter::var(const RegVal* rv) {
  if (rv->rlet_constraint().has_value()) {
    unsupported("variable rlet con registro fijo (ensamblador en línea)");
  }
  if (rv->forced_on_stack()) {
    unsupported("variable en la pila (se toma su dirección)");
  }
  auto id = rv->ireg().id;
  auto t = ctype(rv);
  auto it = m_vars.find(id);
  if (it == m_vars.end()) {
    m_vars[id] = t;
  } else {
    ASSERT(it->second == t);
  }
  return (t == "float" ? "f" : "r") + std::to_string(id);
}

std::string CFunctionWriter::label(int ir_idx) {
  m_label_targets.insert(ir_idx);
  return "L" + std::to_string(ir_idx);
}

std::string CFunctionWriter::symbol(const std::string& name) {
  auto it = m_file_symbols->find(name);
  int idx;
  if (it == m_file_symbols->end()) {
    idx = (int)m_file_symbols->size();
    (*m_file_symbols)[name] = idx;
  } else {
    idx = it->second;
  }
  return fmt::format("g2c_sym[{}] /* {} */", idx, escape_c_comment(name));
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
  } else {
    unsupported("reg_set entre clases no soportadas");
  }
}

std::string CFunctionWriter::finish(const std::string& c_name) {
  std::string out;
  // static: cada fichero solo exporta su tabla de funciones y su función de enlace
  out += fmt::format("static g2c_gpr {}(", c_name);
  for (size_t i = 0; i < params.size(); i++) {
    out += fmt::format("{}g2c_gpr a{}", i ? ", " : "", i);
  }
  if (params.empty()) {
    out += "void";
  }
  out += ") {\n";

  // declaraciones (todas al principio: los goto no pueden saltarse inicializaciones)
  for (auto& [id, t] : m_vars) {
    out += fmt::format("  {} {}{} = 0;\n", t, t == "float" ? "f" : "r", id);
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
  return fe->name();
}

std::string translate_function(FunctionEnv* fe,
                               const std::string& c_name,
                               std::map<std::string, int>* syms,
                               int* nargs) {
  if (fe->is_asm_func) {
    unsupported("asm-func");
  }
  CFunctionWriter w(fe, syms);
  auto& code = fe->code();
  for (size_t i = 0; i < code.size(); i++) {
    w.begin_ir((int)i);
    code[i]->do_codegen_c(w);
  }
  *nargs = (int)w.params.size();
  return w.finish(c_name);
}

}  // namespace

FileResult emit_file(FileEnv* env, const std::string& obj_name) {
  FileResult result;
  std::map<std::string, int> syms;
  std::string functions;
  std::vector<std::pair<std::string, std::string>> table;  // nombre GOAL, nombre C
  std::vector<int> table_nargs;
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

    auto syms_backup = syms;
    try {
      int nargs = 0;
      auto text = translate_function(fe, c_name, &syms, &nargs);
      functions += fmt::format("/* {} */\n", escape_c_comment(goal_name));
      functions += text + "\n";
      table.emplace_back(goal_name, c_name);
      table_nargs.push_back(nargs);
      result.translated.push_back(goal_name);
    } catch (Unsupported& e) {
      syms = syms_backup;
      result.skipped.push_back(fmt::format("{}: {}", goal_name, e.what()));
      functions += fmt::format("/* NO TRADUCIDA: {} -- {} */\n\n", escape_c_comment(goal_name),
                               escape_c_comment(e.what()));
    }
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
  std::vector<std::string> sym_names(syms.size());
  for (auto& [name, idx] : syms) {
    sym_names.at(idx) = name;
  }
  out += fmt::format("static uint32_t g2c_sym[{}];\n", std::max<size_t>(1, sym_names.size()));
  out += fmt::format("void {}_link(void) {{\n", file_c);
  for (size_t i = 0; i < sym_names.size(); i++) {
    out += fmt::format("  g2c_sym[{}] = g2c_rt_symbol(\"{}\");\n", i, sym_names[i]);
  }
  out += "}\n\n";
  out += functions;
  out += fmt::format("const g2c_func_entry {}_funcs[] = {{\n", file_c);
  for (size_t i = 0; i < table.size(); i++) {
    out += fmt::format("  {{\"{}\", (void*){}, {}}},\n", table[i].first, table[i].second,
                       table_nargs[i]);
  }
  out += "  {0, 0, 0}\n};\n";
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
  if (!w.is_gpr(m_dest)) {
    unsupported("puntero a símbolo en registro no GPR");
  }
  w.line(fmt::format("{} = {};", w.var(m_dest), w.symbol(m_name)));
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
  } else {
    unsupported("store desde registro no soportado");
  }
}
