/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0. */
#include "test_main.h"

namespace huxtest {

int g_failures = 0;
std::string g_current;
bool g_skipped = false;
static int g_skip_count = 0;
static std::string g_skip_reason;

std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

void report_skip(std::string const& why) {
  g_skipped = true;
  g_skip_reason = why;
}

void report_failure(char const* file, int line, std::string const& what) {
  ++g_failures;
  std::printf("  FAIL %s\n       %s:%d  %s\n", g_current.c_str(), file, line,
              what.c_str());
}

int run_all() {
  int failed_cases = 0;
  for (auto& c : registry()) {
    g_current = c.name;
    g_skipped = false;
    g_skip_reason.clear();
    int before = g_failures;
    c.fn();
    if (g_failures != before) {
      ++failed_cases;
    } else if (g_skipped) {
      ++g_skip_count;
      std::printf("  skip %s  (%s)\n", c.name.c_str(), g_skip_reason.c_str());
    } else {
      std::printf("  ok   %s\n", c.name.c_str());
    }
  }
  std::printf("\n%zu cases, %d failed, %d skipped (%d checks failed)\n",
              registry().size(), failed_cases, g_skip_count, g_failures);
  return failed_cases == 0 ? 0 : 1;
}

}  // namespace huxtest

int main() { return huxtest::run_all(); }
