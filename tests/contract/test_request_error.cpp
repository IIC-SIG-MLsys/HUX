/* Copyright (c) 2026 IIC-SIG-MLsys. Licensed under the Apache License 2.0.
 *
 * Reading a request's error while another thread fails it. error() returned
 * the member itself, so a caller checking it on a request still in flight
 * read a string while the failing thread assigned it; run under
 * ThreadSanitizer this reported the race, and without it the read could
 * come back torn. */
#include <atomic>
#include <string>
#include <thread>

#include "core/request_impl.h"
#include "test_main.h"

using namespace hux;

HUX_TEST(an_error_read_while_the_request_fails_is_never_torn) {
  for (int round = 0; round < 200; ++round) {
    RequestImpl req(1, SubOp::Kind::kRead, 1, nullptr);
    std::atomic<bool> go{false};
    size_t seen = 0;
    Status seen_status = Status::kOk;
    std::thread reader([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      bool done = false;
      while (!done) {
        ErrorInfo const& e = req.error();
        /* The status too: the string's own copy runs inside the standard
         * library, which the sanitizer does not see, and the plain field is
         * what lets it see this race at all. */
        seen_status = e.status;
        seen = e.detail.size();
        req.test(&done);
      }
    });
    ErrorInfo e;
    e.status = Status::kTransportError;
    e.detail = std::string(200, 'x'); /* long enough to be heap allocated */
    go.store(true, std::memory_order_release);
    req.fail(e);
    reader.join();
    CHECK(seen == 0 || seen == 200);
    CHECK(seen_status == Status::kOk || seen_status == Status::kTransportError);
    CHECK_EQ(req.error().detail.size(), size_t(200));
    CHECK_STATUS(req.error().status, Status::kTransportError);
  }
}
