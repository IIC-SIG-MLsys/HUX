/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
/* Minimal harness: core must build and self-test on a machine without any
 * GPU or RDMA SDK, so no external test dependency is pulled in. */
#ifndef HUX_TESTS_TEST_MAIN_H
#define HUX_TESTS_TEST_MAIN_H

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace huxtest {

struct Case {
  std::string name;
  std::function<void()> fn;
};

std::vector<Case>& registry();
int run_all();

struct Registrar {
  Registrar(char const* name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
  }
};

extern int g_failures;
extern std::string g_current;
extern bool g_skipped;

void report_failure(char const* file, int line, std::string const& what);
void report_skip(std::string const& why);

}  // namespace huxtest

#define HUX_TEST(name)                                 \
  static void name();                                  \
  static ::huxtest::Registrar reg_##name(#name, name); \
  static void name()

/* Marks the case as skipped and returns. A case that silently returns when its
 * hardware is absent would be reported as passing, which is how a whole suite
 * comes to mean nothing on a machine without the device. */
#define SKIP(why)                \
  do {                           \
    ::huxtest::report_skip(why); \
    return;                      \
  } while (0)

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      ::huxtest::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
      return;                                                            \
    }                                                                    \
  } while (0)

#define CHECK_EQ(a, b)                                       \
  do {                                                       \
    auto const& _a = (a);                                    \
    auto const& _b = (b);                                    \
    if (!(_a == _b)) {                                       \
      ::huxtest::report_failure(__FILE__, __LINE__,          \
                                "CHECK_EQ(" #a ", " #b ")"); \
      return;                                                \
    }                                                        \
  } while (0)

#define CHECK_STATUS(expr, expected)                                   \
  do {                                                                 \
    ::hux::Status _s = (expr);                                         \
    if (_s != (expected)) {                                            \
      ::huxtest::report_failure(__FILE__, __LINE__,                    \
                                std::string(#expr " -> ") +            \
                                    ::hux::to_string(_s) + ", want " + \
                                    ::hux::to_string(expected));       \
      return;                                                          \
    }                                                                  \
  } while (0)

#endif  // HUX_TESTS_TEST_MAIN_H
