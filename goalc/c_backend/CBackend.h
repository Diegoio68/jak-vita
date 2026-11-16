/*!
 * @file CBackend.h
 * goal2c (Jak Vita): backend que traduce el IR de goalc a C.
 *
 * Se engancha después del front-end (compile_object_file) y ANTES del register allocator:
 * cada registro virtual del IR (IRegister) pasa a ser una variable local de C y los saltos
 * a etiquetas pasan a ser goto. El reparto de registros lo hace el compilador de C.
 *
 * Uso desde GOAL: (asm-file "goal_src/.../math.gc" :goal2c "out/goal2c/math.c")
 */
#pragma once

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

class FileEnv;
class FunctionEnv;
class RegVal;
struct Label;

namespace goal2c {

/*!
 * Recoge el C de una función mientras se recorre su IR. Cada IR::do_codegen_c escribe
 * aquí sus líneas; el writer lleva la cuenta de variables, etiquetas y símbolos usados.
 */
class CFunctionWriter {
 public:
  CFunctionWriter(const FunctionEnv* env, std::map<std::string, int>* file_symbols);

  //! Nombre de la variable C de un registro virtual (y la declara).
  std::string var(const RegVal* rv);
  //! Tipo C de un registro virtual ("g2c_gpr" o "float"); lanza si no está soportado.
  std::string ctype(const RegVal* rv) const;
  bool is_gpr(const RegVal* rv) const;
  bool is_float(const RegVal* rv) const;
  //! Etiqueta C para un índice de instrucción IR (y la marca como destino de salto).
  std::string label(int ir_idx);
  //! Índice del símbolo en la tabla del fichero: g2c_sym[i] es su dirección GOAL.
  std::string symbol(const std::string& name);
  //! Empieza la traducción de la instrucción IR ir_idx.
  void begin_ir(int ir_idx);
  //! Añade una línea de C al cuerpo.
  void line(const std::string& text);
  //! Copia src en dest convirtiendo entre clases (GPR <-> float) como hace goalc.
  void reg_set(const RegVal* dest, const RegVal* src);

  //! Índice de la instrucción IR que se está traduciendo.
  int current_ir = 0;
  //! Registro de retorno (lo fija IR_Return).
  const RegVal* return_reg = nullptr;
  //! Parámetros, en orden (los fija el IR_ValueReset inicial).
  std::vector<const RegVal*> params;

  //! Genera el texto completo de la función.
  std::string finish(const std::string& c_name);

 private:
  const FunctionEnv* m_env;
  std::map<std::string, int>* m_file_symbols;
  std::map<int, std::string> m_vars;  // ireg id -> tipo C
  std::set<int> m_label_targets;
  // cuerpo: (índice IR, texto)
  std::vector<std::pair<int, std::string>> m_body;
};

struct FileResult {
  std::string c_source;
  std::vector<std::string> translated;  // funciones traducidas
  std::vector<std::string> skipped;     // "nombre: motivo"
};

//! Traduce todas las funciones (no top-level) de un FileEnv a un fichero C.
FileResult emit_file(FileEnv* env, const std::string& obj_name);

//! Convierte un nombre GOAL en un identificador C válido.
std::string mangle(const std::string& goal_name);

}  // namespace goal2c
