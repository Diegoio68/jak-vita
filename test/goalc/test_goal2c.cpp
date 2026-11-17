/*!
 * goal2c (Jak Vita): valida el C generado por goal2c contra goalc (x86).
 *
 * 1. Arranca el runtime real con el motor (ENGINE) cargado, como WithGameTests.
 * 2. Transpila goal_src/jak1/engine/math/math.gc con (asm-file ... :goal2c ...).
 * 3. Compila el C generado con el compilador de C del sistema (cc) junto al arnés
 *    test/goal2c/harness.c, en dos variantes: GPR de 64 bits (la real) y de 32 bits
 *    (solo informativa, para el estudio de coste u64/u32).
 * 4. Ejecuta los casos de test/goal2c/math_cases.txt en el runtime (código x86 de goalc)
 *    y en el arnés, y compara los resultados bit a bit.
 * 5. Escribe out/goal2c/jak1/math_expected.h con los resultados de referencia, que usa el
 *    benchmark de la Vita (vita/bench) para comprobar el C generado en ARM.
 *
 * Ejecutar: build/goalc-test --gtest_filter='Goal2CTests.*'
 */
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

#include "common/util/FileUtil.h"

#include "game/goal2c/goal2c_jak1.h"
#include "game/runtime.h"
#include "goalc/compiler/Compiler.h"
#include "gtest/gtest.h"
#include "test/goalc/framework/test_runner.h"

#include "fmt/format.h"

namespace {

struct Case {
  std::string fn;
  std::string kind;                    // int | float | bool
  std::vector<u64> args;               // valores de registro GOAL
  std::vector<std::string> goal_args;  // mismos argumentos como expresión GOAL
  std::string text;                    // línea original, para los mensajes
};

std::vector<Case> load_cases(const fs::path& path) {
  std::vector<Case> cases;
  std::istringstream in(file_util::read_text_file(path));
  std::string line;
  while (std::getline(in, line)) {
    auto hash = line.find('#');
    auto content = hash == 0 ? std::string() : line;
    if (content.find_first_not_of(" \t\r") == std::string::npos) {
      continue;
    }
    // "#" solo es comentario al principio de línea (integral? lleva "?" y no "#")
    std::istringstream ls(content);
    Case c;
    c.text = content;
    ls >> c.fn >> c.kind;
    std::string tok;
    while (ls >> tok) {
      if (tok.rfind("i:", 0) == 0) {
        s64 v = std::stoll(tok.substr(2));
        c.args.push_back((u64)v);
        c.goal_args.push_back(std::to_string(v));
      } else if (tok.rfind("f:", 0) == 0) {
        float f = std::stof(tok.substr(2));
        u32 bits;
        memcpy(&bits, &f, 4);
        c.args.push_back(bits);
        c.goal_args.push_back(fmt::format("(the-as float #x{:x})", bits));
      } else {
        throw std::runtime_error("argumento inválido: " + tok);
      }
    }
    cases.push_back(c);
  }
  return cases;
}

//! Ejecuta un comando y devuelve su salida estándar.
std::string run_capture(const std::string& cmd) {
  std::string out;
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) {
    return out;
  }
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), p)) > 0) {
    out.append(buf, n);
  }
  pclose(p);
  return out;
}

//! Normaliza un resultado según el tipo, para comparar.
u64 normalize(const std::string& kind, u64 raw, u64 sym_t, u64 sym_f, bool* ok) {
  *ok = true;
  if (kind == "float") {
    return raw & 0xffffffffULL;
  } else if (kind == "bool") {
    if (raw == sym_t) {
      return 1;
    }
    if (raw == sym_f) {
      return 0;
    }
    *ok = false;
    return raw;
  }
  return raw;
}

//! Ejecuta los casos en el runtime (lo que haya en cada símbolo: x86 de goalc o C de
//! goal2c en modo mixto) y devuelve el valor de cada uno. pc-rand es aleatoria de verdad: se
//! sustituye por la misma secuencia determinista que usa el arnés (contador * 7919).
std::vector<std::optional<u64>> run_cases_in_runtime(Compiler& compiler,
                                                     const std::vector<Case>& cases) {
  compiler.run_test_from_string(
      "(define *g2c-saved-pc-rand* pc-rand)\n"
      "(define *g2c-pc-rand-counter* 0)\n"
      "(set! pc-rand (the-as (function int) (lambda () (set! *g2c-pc-rand-counter* (+ "
      "*g2c-pc-rand-counter* 1)) (* *g2c-pc-rand-counter* 7919))))\n");
  std::string goal_prog = "(begin\n";
  for (size_t i = 0; i < cases.size(); i++) {
    auto& c = cases[i];
    std::string call = "(" + c.fn;
    for (auto& a : c.goal_args) {
      call += " " + a;
    }
    call += ")";
    std::string value;
    if (c.kind == "bool") {
      value = fmt::format("(if {} 1 0)", call);
    } else {
      value = fmt::format("(the-as int {})", call);  // float -> sus bits (movd)
    }
    goal_prog += fmt::format("  (format #t \"G2C ~D ~D~%\" {} {})\n", i, value);
  }
  goal_prog += "  0)\n";
  auto msgs = compiler.run_test_from_string(goal_prog);
  compiler.run_test_from_string("(set! pc-rand *g2c-saved-pc-rand*)");
  std::string text;
  for (auto& m : msgs) {
    text += m;
  }
  std::vector<std::optional<u64>> result(cases.size());
  std::istringstream rs(text);
  std::string tag;
  while (rs >> tag) {
    if (tag != "G2C") {
      continue;
    }
    size_t idx;
    s64 v;
    rs >> idx >> v;
    if (idx < cases.size()) {
      result[idx] = (u64)v;
    }
  }
  return result;
}

//! Salida de inspect sobre valores fijos: usa strings estáticos del segmento de depuración
//! y llama a format (función C del kernel) desde el método inspect.
std::string run_inspect_in_runtime(Compiler& compiler) {
  auto msgs = compiler.run_test_from_string(
      "(begin (inspect (the-as rgba #x04030201)) (inspect (the-as rgba #xff80407f)) 0)");
  std::string text;
  for (auto& m : msgs) {
    text += m;
  }
  return text;
}

}  // namespace

//! Diagnóstico: si el runtime muere por SIGFPE, decir dónde (y si es división entera).
void g2c_fpe_handler(int sig, siginfo_t* info, void* vctx) {
  auto* uc = (ucontext_t*)vctx;
  void* rip = (void*)uc->uc_mcontext.gregs[REG_RIP];
  Dl_info d{};
  char buf[512];
  int n;
  if (dladdr(rip, &d) && d.dli_fname) {
    n = snprintf(buf, sizeof(buf),
                 "[goal2c] SIGFPE code=%d (1=div entera) en %p (%s + 0x%lx, %s)\n", info->si_code,
                 rip, d.dli_fname, (unsigned long)((u8*)rip - (u8*)d.dli_fbase),
                 d.dli_sname ? d.dli_sname : "?");
  } else {
    n = snprintf(buf, sizeof(buf),
                 "[goal2c] SIGFPE code=%d (1=div entera) en %p (código GOAL, offset EE 0x%lx)\n",
                 info->si_code, rip, (unsigned long)((u8*)rip - g_ee_main_mem));
  }
  (void)!write(2, buf, (size_t)n);
  signal(sig, SIG_DFL);
  raise(sig);
}

class Goal2CTests : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    struct sigaction sa{};
    sa.sa_sigaction = g2c_fpe_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGFPE, &sa, nullptr);
    shared = std::make_unique<Shared>();
    shared->compiler.run_test_no_load("test/goalc/source_templates/with_game/test-build-game.gc");
    shared->runtime_thread = std::thread(GoalTest::runtime_with_kernel_jak1);
    shared->compiler.run_test_from_file("test/goalc/source_templates/with_game/test-load-game.gc");
    shared->compiler.run_test_from_string("(set! *use-old-listener-print* #t)");
  }
  static void TearDownTestSuite() {
    shared->compiler.shutdown_target();
    shared->runtime_thread.join();
    shared.reset();
  }

  struct Shared {
    Shared() : compiler(GameVersion::Jak1, emitter::kNativeInstructionSet) {}
    std::thread runtime_thread;
    Compiler compiler;
  };
  static std::unique_ptr<Shared> shared;
};

std::unique_ptr<Goal2CTests::Shared> Goal2CTests::shared;

TEST_F(Goal2CTests, Math) {
  const auto root = file_util::get_jak_project_dir();
  const auto out_dir = root / "out" / "goal2c" / "jak1";
  const auto c_file = out_dir / "math.c";
  file_util::create_dir_if_needed(out_dir);

  // 1) transpilar
  shared->compiler.run_front_end_on_string(fmt::format(
      "(asm-file \"goal_src/jak1/engine/math/math.gc\" :goal2c \"{}\")", c_file.string()));
  ASSERT_TRUE(fs::exists(c_file));

  // 2) compilar el arnés (64 y 32 bits)
  if (std::system("cc --version > /dev/null 2>&1") != 0) {
    GTEST_SKIP() << "no hay compilador de C (cc) en el sistema";
  }
  auto build = [&](const std::string& exe, const std::string& extra) {
    auto cmd = fmt::format(
        "cc -std=c11 -O2 -Wall -Wextra -Werror {} -I{} -DG2C_TABLE=g2c_math_funcs "
        "-DG2C_LINK=g2c_math_link {} {} -o {} 2>&1",
        extra, (root / "goalc" / "c_backend" / "runtime").string(),
        (root / "test" / "goal2c" / "harness.c").string(), c_file.string(), exe);
    auto out = run_capture(cmd + "; echo EXIT=$?");
    EXPECT_NE(out.find("EXIT=0"), std::string::npos) << cmd << "\n" << out;
  };
  const auto exe64 = (out_dir / "math_harness").string();
  const auto exe32 = (out_dir / "math_harness32").string();
  build(exe64, "");
  build(exe32, "-DG2C_GPR32");

  // 3) casos
  auto cases = load_cases(root / "test" / "goal2c" / "math_cases.txt");
  ASSERT_FALSE(cases.empty());
  std::string harness_in;
  for (auto& c : cases) {
    harness_in += c.fn;
    for (auto a : c.args) {
      harness_in += fmt::format(" {:x}", a);
    }
    harness_in += "\n";
  }
  const auto in_file = out_dir / "math_cases.in";
  file_util::write_text_file(in_file, harness_in);

  // 4) referencia: el mismo cálculo en el runtime, con el código x86 de goalc
  auto ref_opt = run_cases_in_runtime(shared->compiler, cases);
  std::vector<u64> ref(cases.size(), 0);
  std::vector<bool> have_ref(cases.size(), false);
  for (size_t i = 0; i < cases.size(); i++) {
    have_ref[i] = ref_opt[i].has_value();
    ref[i] = ref_opt[i].value_or(0);
  }

  // 5) ejecutar el arnés y comparar
  auto run_harness = [&](const std::string& exe, bool is32, int* mismatches) {
    auto out = run_capture(fmt::format("{} < {}", exe, in_file.string()));
    std::istringstream hs(out);
    std::string line;
    u64 sym_t = 0, sym_f = 0;
    size_t idx = 0;
    *mismatches = 0;
    while (std::getline(hs, line)) {
      std::istringstream ls(line);
      std::string tag;
      ls >> tag;
      if (tag == "SYM") {
        std::string name, addr;
        ls >> name >> addr;
        (name == "#t" ? sym_t : sym_f) = std::stoull(addr, nullptr, 16);
      } else if (tag == "RET" && idx < cases.size()) {
        std::string hex;
        ls >> hex;
        u64 raw = std::stoull(hex, nullptr, 16);
        auto& c = cases[idx];
        if (is32 && c.kind == "int") {
          raw = (u64)(s64)(s32)(u32)raw;  // 32 bits con signo, para comparar
        }
        bool ok;
        u64 got = normalize(c.kind, raw, sym_t, sym_f, &ok);
        u64 want = c.kind == "float" ? (ref[idx] & 0xffffffffULL) : ref[idx];
        bool match = ok && have_ref[idx] && got == want;
        if (!match) {
          (*mismatches)++;
          if (!is32) {
            ADD_FAILURE() << fmt::format("caso {}: '{}' goalc={:#x} goal2c={:#x}", idx, c.text,
                                         want, got);
          } else {
            lg::print("[goal2c] u32 difiere en caso {}: '{}' goalc={:#x} goal2c32={:#x}\n", idx,
                      c.text, want, got);
          }
        }
        idx++;
      } else if (tag == "ERR") {
        ADD_FAILURE() << "arnés: " << line;
        idx++;
      }
    }
    EXPECT_EQ(idx, cases.size()) << exe << " no respondió a todos los casos";
  };

  for (size_t i = 0; i < cases.size(); i++) {
    EXPECT_TRUE(have_ref[i]) << "sin referencia de goalc para el caso " << i << ": "
                             << cases[i].text;
  }
  int bad64 = 0, bad32 = 0;
  run_harness(exe64, false, &bad64);
  run_harness(exe32, true, &bad32);
  lg::print("[goal2c] math: {} casos, {} difieren con u64, {} difieren con u32\n", cases.size(),
            bad64, bad32);

  // 6) referencia para el benchmark de la Vita
  std::string h = "/* Generado por test/goalc/test_goal2c.cpp. NO EDITAR A MANO. */\n";
  h += "/* Resultados de goalc (x86) para test/goal2c/math_cases.txt */\n";
  h += "static const struct { const char* fn; const char* kind; int nargs; ";
  h += "unsigned long long args[8]; unsigned long long expected; } g2c_math_expected[] = {\n";
  for (size_t i = 0; i < cases.size(); i++) {
    auto& c = cases[i];
    std::string args;
    for (size_t j = 0; j < 8; j++) {
      args += fmt::format("{}0x{:x}ULL", j ? ", " : "", j < c.args.size() ? c.args[j] : 0);
    }
    u64 want = c.kind == "float" ? (ref[i] & 0xffffffffULL) : ref[i];
    h += fmt::format("  {{\"{}\", \"{}\", {}, {{{}}}, 0x{:x}ULL}},\n", c.fn, c.kind, c.args.size(),
                     args, want);
  }
  h += "};\n";
  file_util::write_text_file(out_dir / "math_expected.h", h);

  // 7) referencia para Goal2CMixedTests (el mismo runtime con las funciones en C)
  std::string ref_txt;
  for (size_t i = 0; i < cases.size(); i++) {
    ref_txt += fmt::format("{} {}\n", i, have_ref[i] ? std::to_string((s64)ref[i]) : "-");
  }
  file_util::write_text_file(out_dir / "math_reference.txt", ref_txt);
  auto inspect_out = run_inspect_in_runtime(shared->compiler);
  EXPECT_NE(inspect_out.find("rgba"), std::string::npos) << inspect_out;
  file_util::write_text_file(out_dir / "math_inspect_reference.txt", inspect_out);
}

//! Junta los mensajes de un test en un texto y quita los saltos de línea finales.
std::string join_output(const std::vector<std::string>& msgs) {
  std::string text;
  for (auto& m : msgs) {
    text += m;
  }
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

//! Ejecuta en el runtime un fichero de casos (test/goal2c/<nombre>_cases.gc) precedido del
//! preludio común y seguido de la restauración de pc-rand. Devuelve la salida de texto.
std::string run_cases_file(Compiler& compiler, const std::string& name) {
  const auto dir = file_util::get_jak_project_dir() / "test" / "goal2c";
  std::string src = file_util::read_text_file(dir / "cases_prelude.gc") + "\n" +
                    file_util::read_text_file(dir / (name + "_cases.gc")) + "\n" +
                    "(set! pc-rand *g2c-saved-pc-rand*)\n";
  return join_output(compiler.run_test_from_string(src, "g2c-" + name + "-cases"));
}

//! Ficheros validados en el juego: objeto, fuente GOAL y un texto que debe salir en los casos.
struct Goal2CFile {
  const char* obj;
  const char* src;
  const char* marker;
};
const Goal2CFile kGameFiles[] = {
    {"vector", "goal_src/jak1/engine/math/vector.gc", "\"cross\" "},
    {"matrix", "goal_src/jak1/engine/math/matrix.gc", "\"4x4-inverse\" "},
    {"transform", "goal_src/jak1/engine/math/transform.gc", "\"matrix-calc\" "},
    {"quaternion", "goal_src/jak1/engine/math/quaternion.gc", "\"slerp\" "},
    {"trigonometry", "goal_src/jak1/engine/math/trigonometry.gc", "\"atan\" "},
    {"euler", "goal_src/jak1/engine/math/euler.gc", "\"eul->matrix\" "},
    {"geometry", "goal_src/jak1/engine/geometry/geometry.gc", "\"flatten\" "},
    {"bounding-box", "goal_src/jak1/engine/geometry/bounding-box.gc", "\"add-box\" "},
    {"cylinder", "goal_src/jak1/engine/geometry/cylinder.gc", "\"capsule-1\" "},
    {"collide-func", "goal_src/jak1/engine/collide/collide-func.gc", "\"ray-sphere\" "},
};

//! Ficheros validados solo dentro del juego (necesitan niveles, procesos, colisión real...):
//! objeto idéntico, C que compila limpio, funciones instaladas por el runtime y la prueba de
//! humo / comprobación de colisión con el juego real (test/goal2c/boot_smoke.sh).
const Goal2CFile kGameOnlyFiles[] = {
    {"bounding-box-h", "goal_src/jak1/engine/geometry/bounding-box-h.gc", ""},
    {"geometry-h", "goal_src/jak1/engine/geometry/geometry-h.gc", ""},
    {"surface-h", "goal_src/jak1/engine/collide/surface-h.gc", ""},
    {"pat-h", "goal_src/jak1/engine/collide/pat-h.gc", ""},
    {"collide-func-h", "goal_src/jak1/engine/collide/collide-func-h.gc", ""},
    {"collide-mesh-h", "goal_src/jak1/engine/collide/collide-mesh-h.gc", ""},
    {"collide-shape-h", "goal_src/jak1/engine/collide/collide-shape-h.gc", ""},
    {"collide-target-h", "goal_src/jak1/engine/collide/collide-target-h.gc", ""},
    {"collide-touch-h", "goal_src/jak1/engine/collide/collide-touch-h.gc", ""},
    {"collide-edge-grab-h", "goal_src/jak1/engine/collide/collide-edge-grab-h.gc", ""},
    {"collide-frag-h", "goal_src/jak1/engine/collide/collide-frag-h.gc", ""},
    {"collide-cache-h", "goal_src/jak1/engine/collide/collide-cache-h.gc", ""},
    {"collide-h", "goal_src/jak1/engine/collide/collide-h.gc", ""},
    {"path-h", "goal_src/jak1/engine/geometry/path-h.gc", ""},
    {"collide-probe", "goal_src/jak1/engine/collide/collide-probe.gc", ""},
    {"collide-frag", "goal_src/jak1/engine/collide/collide-frag.gc", ""},
    {"collide-mesh", "goal_src/jak1/engine/collide/collide-mesh.gc", ""},
    {"collide-touch", "goal_src/jak1/engine/collide/collide-touch.gc", ""},
    {"collide-edge-grab", "goal_src/jak1/engine/collide/collide-edge-grab.gc", ""},
    {"collide-shape", "goal_src/jak1/engine/collide/collide-shape.gc", ""},
    {"collide-shape-rider", "goal_src/jak1/engine/collide/collide-shape-rider.gc", ""},
    {"collide", "goal_src/jak1/engine/collide/collide.gc", ""},
    // collide-planes no está: aparece en all_objs.json pero no tiene fuente .gc en OpenGOAL
    {"vol-h", "goal_src/jak1/engine/geometry/vol-h.gc", ""},
    {"main-collide", "goal_src/jak1/engine/collide/main-collide.gc", ""},
    {"collide-cache", "goal_src/jak1/engine/collide/collide-cache.gc", ""},
    {"path", "goal_src/jak1/engine/geometry/path.gc", ""},
    {"vol", "goal_src/jak1/engine/geometry/vol.gc", ""},
};

//! Transpila el fichero, comprueba que su C compila limpio y guarda la referencia de goalc
//! (los casos ejecutados con el código x86 de goalc) en out/goal2c/jak1/<obj>_reference.txt.
void transpile_and_reference(Compiler& compiler, const Goal2CFile& f) {
  const auto root = file_util::get_jak_project_dir();
  const auto out_dir = root / "out" / "goal2c" / "jak1";
  const auto c_file = out_dir / (std::string(f.obj) + ".c");
  compiler.run_front_end_on_string(
      fmt::format("(asm-file \"{}\" :goal2c \"{}\")", f.src, c_file.string()));
  ASSERT_TRUE(fs::exists(c_file));
  // el C debe compilar limpio (también como código de la Vita, sin modo mixto)
  auto out = run_capture(
      fmt::format("cc -std=c11 -O2 -Wall -Wextra -Werror -c -I{} {} -o {} 2>&1; echo EXIT=$?",
                  (root / "goalc" / "c_backend" / "runtime").string(), c_file.string(),
                  (out_dir / (std::string(f.obj) + "_check.o")).string()));
  EXPECT_NE(out.find("EXIT=0"), std::string::npos) << out;

  auto ref = run_cases_file(compiler, f.obj);
  EXPECT_NE(ref.find(f.marker), std::string::npos) << ref;
  file_util::write_text_file(out_dir / (std::string(f.obj) + "_reference.txt"), ref);
}

TEST_F(Goal2CTests, Vector) {
  transpile_and_reference(shared->compiler, kGameFiles[0]);
}
TEST_F(Goal2CTests, Matrix) {
  transpile_and_reference(shared->compiler, kGameFiles[1]);
}
TEST_F(Goal2CTests, Transform) {
  transpile_and_reference(shared->compiler, kGameFiles[2]);
}
TEST_F(Goal2CTests, Quaternion) {
  transpile_and_reference(shared->compiler, kGameFiles[3]);
}
TEST_F(Goal2CTests, Trigonometry) {
  transpile_and_reference(shared->compiler, kGameFiles[4]);
}
TEST_F(Goal2CTests, Euler) {
  transpile_and_reference(shared->compiler, kGameFiles[5]);
}
TEST_F(Goal2CTests, Geometry) {
  transpile_and_reference(shared->compiler, kGameFiles[6]);
}
TEST_F(Goal2CTests, BoundingBox) {
  transpile_and_reference(shared->compiler, kGameFiles[7]);
}
TEST_F(Goal2CTests, Cylinder) {
  transpile_and_reference(shared->compiler, kGameFiles[8]);
}
TEST_F(Goal2CTests, CollideFunc) {
  transpile_and_reference(shared->compiler, kGameFiles[9]);
}
//! collide/geometry sin casos propios: transpilar y comprobar que el C compila limpio.
TEST_F(Goal2CTests, CollideGeometryGameOnly) {
  const auto root = file_util::get_jak_project_dir();
  const auto out_dir = root / "out" / "goal2c" / "jak1";
  for (auto& f : kGameOnlyFiles) {
    const auto c_file = out_dir / (std::string(f.obj) + ".c");
    shared->compiler.run_front_end_on_string(
        fmt::format("(asm-file \"{}\" :goal2c \"{}\")", f.src, c_file.string()));
    ASSERT_TRUE(fs::exists(c_file)) << f.obj;
    auto out = run_capture(
        fmt::format("cc -std=c11 -O2 -Wall -Wextra -Werror -c -I{} {} -o {} 2>&1; echo EXIT=$?",
                    (root / "goalc" / "c_backend" / "runtime").string(), c_file.string(),
                    (out_dir / (std::string(f.obj) + "_check.o")).string()));
    EXPECT_NE(out.find("EXIT=0"), std::string::npos) << f.obj << "\n" << out;
  }
}

/*!
 * El juego real en modo mixto: el runtime carga la librería de goal2c (GOAL2C_LIB) y, al
 * enlazar math, sustituye sus funciones x86 por las de C. Los mismos casos se ejecutan desde
 * GOAL (código x86 del test que llama a las funciones en C, que a su vez llaman a pc-rand y
 * a format en el runtime) y deben dar exactamente lo mismo que con goalc.
 * Depende de Goal2CTests.Math, que genera math.c y la referencia.
 */
class Goal2CMixedTests : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    const auto root = file_util::get_jak_project_dir();
    const auto out_dir = root / "out" / "goal2c" / "jak1";
    lib_path = (out_dir / "libgoal2c_jak1.so").string();
    auto cmd = fmt::format("python3 {} --dir {} --out {} 2>&1; echo EXIT=$?",
                           (root / "goalc" / "c_backend" / "tools" / "build_pc_lib.py").string(),
                           out_dir.string(), lib_path);
    lib_build_output = run_capture(cmd);
    setenv("GOAL2C_LIB", lib_path.c_str(), 1);
    shared = std::make_unique<Goal2CTests::Shared>();
    shared->compiler.run_test_no_load("test/goalc/source_templates/with_game/test-build-game.gc");
    shared->runtime_thread = std::thread(GoalTest::runtime_with_kernel_jak1);
    shared->compiler.run_test_from_file("test/goalc/source_templates/with_game/test-load-game.gc");
    shared->compiler.run_test_from_string("(set! *use-old-listener-print* #t)");
  }
  static void TearDownTestSuite() {
    shared->compiler.shutdown_target();
    shared->runtime_thread.join();
    shared.reset();
    unsetenv("GOAL2C_LIB");
  }
  static std::unique_ptr<Goal2CTests::Shared> shared;
  static std::string lib_path;
  static std::string lib_build_output;
};

std::unique_ptr<Goal2CTests::Shared> Goal2CMixedTests::shared;
std::string Goal2CMixedTests::lib_path;
std::string Goal2CMixedTests::lib_build_output;

TEST_F(Goal2CMixedTests, MathInGame) {
  const auto out_dir = file_util::get_jak_project_dir() / "out" / "goal2c" / "jak1";
  ASSERT_NE(lib_build_output.find("EXIT=0"), std::string::npos) << lib_build_output;
  ASSERT_TRUE(fs::exists(out_dir / "math_reference.txt")) << "ejecuta antes Goal2CTests.Math";

  // el objeto que transpiló goal2c es el mismo que carga el juego
  EXPECT_EQ(file_util::read_binary_file(out_dir / "math.c.o"),
            file_util::read_binary_file(file_util::get_jak_project_dir() / "out" / "jak1" / "obj" /
                                        "math.o"));
  // todas las funciones de math están en C
  EXPECT_EQ(goal2c_runtime::replaced_functions("math"), 19);

  auto cases = load_cases(file_util::get_jak_project_dir() / "test" / "goal2c" / "math_cases.txt");
  std::vector<std::string> ref(cases.size());
  {
    std::istringstream rs(file_util::read_text_file(out_dir / "math_reference.txt"));
    size_t idx;
    std::string v;
    while (rs >> idx >> v) {
      if (idx < ref.size()) {
        ref[idx] = v;
      }
    }
  }
  auto got = run_cases_in_runtime(shared->compiler, cases);
  int bad = 0;
  for (size_t i = 0; i < cases.size(); i++) {
    std::string g = got[i] ? std::to_string((s64)*got[i]) : "-";
    if (g != ref[i] || g == "-") {
      bad++;
      ADD_FAILURE() << fmt::format("caso {}: '{}' goalc={} juego-con-goal2c={}", i, cases[i].text,
                                   ref[i], g);
    }
  }
  lg::print("[goal2c] modo mixto: {} casos en el juego, {} difieren\n", cases.size(), bad);

  auto rtrim = [](std::string x) {
    while (!x.empty() && (x.back() == '\n' || x.back() == '\r')) {
      x.pop_back();
    }
    return x;
  };
  auto inspect_out = run_inspect_in_runtime(shared->compiler);
  EXPECT_EQ(rtrim(inspect_out),
            rtrim(file_util::read_text_file(out_dir / "math_inspect_reference.txt")));
  lg::print("[goal2c] inspect en C:\n{}\n", inspect_out);
}

//! El fichero en el juego en modo mixto: objeto idéntico al transpilado, funciones instaladas
//! en C y los mismos casos con la misma salida, línea a línea, que con goalc.
void compare_in_game(Compiler& compiler, const std::string& lib_build_output, const Goal2CFile& f) {
  const auto root = file_util::get_jak_project_dir();
  const auto out_dir = root / "out" / "goal2c" / "jak1";
  const std::string obj = f.obj;
  ASSERT_NE(lib_build_output.find("EXIT=0"), std::string::npos) << lib_build_output;
  ASSERT_TRUE(fs::exists(out_dir / (obj + "_reference.txt")))
      << "ejecuta antes Goal2CTests (genera la referencia de " << obj << ")";
  EXPECT_EQ(file_util::read_binary_file(out_dir / (obj + ".c.o")),
            file_util::read_binary_file(root / "out" / "jak1" / "obj" / (obj + ".o")));
  int replaced = goal2c_runtime::replaced_functions(obj.c_str());
  EXPECT_GT(replaced, 0);
  lg::print("[goal2c] {}: {} funciones en C dentro del juego\n", obj, replaced);

  auto got = run_cases_file(compiler, obj);
  auto ref = file_util::read_text_file(out_dir / (obj + "_reference.txt"));
  while (!ref.empty() && (ref.back() == '\n' || ref.back() == '\r')) {
    ref.pop_back();
  }
  std::istringstream gs(got), rs(ref);
  std::string gl, rl;
  int line = 0, bad = 0;
  while (true) {
    bool g_ok = (bool)std::getline(gs, gl);
    bool r_ok = (bool)std::getline(rs, rl);
    if (!g_ok && !r_ok) {
      break;
    }
    line++;
    if (gl != rl || g_ok != r_ok) {
      bad++;
      ADD_FAILURE() << fmt::format("{} línea {}: goalc='{}' juego-con-goal2c='{}'", obj, line, rl,
                                   gl);
    }
  }
  lg::print("[goal2c] {} en el juego: {} líneas comparadas, {} difieren\n", obj, line, bad);
}

TEST_F(Goal2CMixedTests, VectorInGame) {
  compare_in_game(shared->compiler, lib_build_output, kGameFiles[0]);
}
TEST_F(Goal2CMixedTests, MatrixInGame) {
  compare_in_game(shared->compiler, lib_build_output, kGameFiles[1]);
}
TEST_F(Goal2CMixedTests, TransformInGame) {
  compare_in_game(shared->compiler, lib_build_output, kGameFiles[2]);
}
TEST_F(Goal2CMixedTests, QuaternionInGame) {
  compare_in_game(shared->compiler, lib_build_output, kGameFiles[3]);
}
TEST_F(Goal2CMixedTests, TrigonometryInGame) {
  compare_in_game(shared->compiler, lib_build_output, kGameFiles[4]);
}
TEST_F(Goal2CMixedTests, EulerInGame) {
  compare_in_game(shared->compiler, lib_build_output, kGameFiles[5]);
}
TEST_F(Goal2CMixedTests, GeometryInGame) {
  compare_in_game(shared->compiler, lib_build_output, kGameFiles[6]);
}
TEST_F(Goal2CMixedTests, BoundingBoxInGame) {
  compare_in_game(shared->compiler, lib_build_output, kGameFiles[7]);
}
TEST_F(Goal2CMixedTests, CylinderInGame) {
  compare_in_game(shared->compiler, lib_build_output, kGameFiles[8]);
}
TEST_F(Goal2CMixedTests, CollideFuncInGame) {
  compare_in_game(shared->compiler, lib_build_output, kGameFiles[9]);
}
//! Los ficheros sin casos: objeto idéntico al que carga el juego y funciones instaladas.
//! (Las cabeceras -h sin funciones propias no instalan nada: solo se exige el objeto.)
TEST_F(Goal2CMixedTests, CollideGeometryGameOnly) {
  const auto root = file_util::get_jak_project_dir();
  const auto out_dir = root / "out" / "goal2c" / "jak1";
  int total = 0;
  for (auto& f : kGameOnlyFiles) {
    const std::string obj = f.obj;
    EXPECT_EQ(file_util::read_binary_file(out_dir / (obj + ".c.o")),
              file_util::read_binary_file(root / "out" / "jak1" / "obj" / (obj + ".o")))
        << obj;
    total += goal2c_runtime::replaced_functions(obj.c_str());
  }
  lg::print("[goal2c] collide/geometry sin casos: {} funciones en C dentro del juego\n", total);
  EXPECT_GT(total, 0);
}
