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

#include "common/common_types.h"

class FileEnv;
class FunctionEnv;
class RegVal;
class StaticObject;
struct Label;
struct IRegConstraint;

namespace goal2c {

/*!
 * Recoge el C de una función mientras se recorre su IR. Cada IR::do_codegen_c escribe
 * aquí sus líneas; el writer lleva la cuenta de variables, etiquetas y símbolos usados.
 */
class CFunctionWriter {
 public:
  //! Estado compartido por todas las funciones de un fichero.
  struct FileState {
    std::map<std::string, int> symbols;             // nombre -> índice en g2c_sym
    std::map<const StaticObject*, int> static_idx;  // objeto estático -> índice en el fichero
    std::set<int> used_statics;                     // estáticos referenciados por el C
    std::map<const FunctionEnv*, int> func_addr_idx;  // función (mov-fa) -> índice en g2c_fn
  };

  CFunctionWriter(const FunctionEnv* env, FileState* file);

  //! Nombre de la variable C de un registro virtual (y la declara).
  std::string var(const RegVal* rv);
  //! Tipo C de un registro virtual ("g2c_gpr" o "float"); lanza si no está soportado.
  std::string ctype(const RegVal* rv) const;
  bool is_gpr(const RegVal* rv) const;
  //! ¿Es la variable el registro pp (r13: proceso actual, self de los behaviors)?
  bool is_pp(const RegVal* rv) const;
  bool is_float(const RegVal* rv) const;
  //! Registro de 128 bits (VECTOR_FLOAT o INT_128): tipo g2c_q.
  bool is_q(const RegVal* rv) const;
  static std::string var_prefix(const std::string& ctype);
  //! Etiqueta C para un índice de instrucción IR (y la marca como destino de salto).
  std::string label(int ir_idx);
  //! Índice del símbolo en la tabla del fichero: g2c_sym[i] es su dirección GOAL.
  std::string symbol(const std::string& name);
  //! Dirección GOAL de un objeto estático del fichero (string, estructura...).
  std::string static_addr(const StaticObject* obj);
  //! Dirección GOAL del objeto función de una función del fichero (lambdas: mov-fa).
  std::string function_addr(const FunctionEnv* fe);
  //! ¿La función escribe pp (suspend, with-proc...)? Ver finish().
  bool writes_pp() const;
  //! Lanza si hay restricciones a registros especiales que el C no sabe representar.
  void check_constraints() const;
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
  //! Dirección GOAL del hueco de pila slot (variables 'stack de GOAL). Reserva el área.
  std::string stack_addr(int slot);

  //! Genera el texto completo de la función.
  std::string finish(const std::string& c_name);

  //! Firma (tras finish): qué parámetros son de 128 bits y si el retorno lo es.
  std::vector<bool> param_is_q;
  bool ret_is_q = false;
  bool has_q_signature() const;

 private:
  const FunctionEnv* m_env;
  FileState* m_file;
  std::map<int, std::string> m_vars;  // ireg id -> tipo C
  std::set<int> m_addr_taken;         // variables cuya dirección se toma (forced on stack)
  // ireg id -> restricción "en toda la función" (rlet :reg y self de los behaviors). Un rlet que
  // reutiliza un registro ya restringido (p. ej. pp en un behavior, o el segundo with-pp de una
  // función) crea una RegVal nueva SIN rlet_constraint: la restricción solo está aquí.
  std::map<int, const IRegConstraint*> m_everywhere;
  std::set<int> m_label_targets;
  bool m_uses_stack = false;
  // cuerpo: (índice IR, texto)
  std::vector<std::pair<int, std::string>> m_body;
};

//! Objeto estático -> (segmento, posición dentro del segmento) en el objeto generado.
using StaticLocations = std::map<const StaticObject*, std::pair<int, int>>;

//! Objeto función en el objeto generado: segmento, posición (primera instrucción) y longitud.
struct FunctionLocation {
  int seg = -1;
  int offset = -1;
  int length = 0;
};
using FunctionLocations = std::map<const FunctionEnv*, FunctionLocation>;

struct FileResult {
  std::string c_source;
  std::vector<std::string> translated;  // funciones traducidas
  std::vector<std::string> skipped;     // "nombre: motivo"
};

//! Traduce todas las funciones (no top-level) de un FileEnv a un fichero C.
//! statics: ubicación de cada estático en el objeto generado por goalc para este fichero.
//! functions: ubicación de cada función en ese objeto (lambdas y parcheo en el modo mixto).
//! object_data: ese objeto (se usa para guardar los tamaños de segmento como huella).
FileResult emit_file(FileEnv* env,
                     const std::string& obj_name,
                     const StaticLocations& statics,
                     const FunctionLocations& functions,
                     const std::vector<u8>& object_data);

//! Convierte un nombre GOAL en un identificador C válido.
std::string mangle(const std::string& goal_name);

}  // namespace goal2c
