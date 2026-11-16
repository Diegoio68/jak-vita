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
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/util/FileUtil.h"

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

}  // namespace

class Goal2CTests : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
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
  auto msgs = shared->compiler.run_test_from_string(goal_prog);
  std::string ref_text;
  for (auto& m : msgs) {
    ref_text += m;
  }
  std::vector<u64> ref(cases.size(), 0);
  std::vector<bool> have_ref(cases.size(), false);
  {
    std::istringstream rs(ref_text);
    std::string tag;
    while (rs >> tag) {
      if (tag != "G2C") {
        continue;
      }
      size_t idx;
      s64 v;
      rs >> idx >> v;
      if (idx < cases.size()) {
        ref[idx] = (u64)v;
        have_ref[idx] = true;
      }
    }
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
}
