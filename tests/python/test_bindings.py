"""Python binding tests, over the mock backend so no hardware is needed.

The cases that matter here are the ones with no Python-level symptom. A
registration that loses its buffer, or a blocking call that keeps the GIL,
fails as a crash or a hang rather than as an exception someone can read.

Run:  python tests/python/test_bindings.py
"""
import gc
import sys
import threading
import time

import hux


FAILURES = []


def check(cond, what):
    if cond:
        print(f"  ok   {what}")
    else:
        print(f"  FAIL {what}")
        FAILURES.append(what)


def setup(move_data=True):
    """An engine whose peer is itself, which is enough to exercise the API."""
    eng = hux.make_mock_engine(move_data=move_data)
    peer = eng.add_peer(eng.local_metadata())
    return eng, peer


def test_round_trip():
    eng, peer = setup()
    src = bytearray(b"".join(bytes([i % 251]) for i in range(4096)))
    dst = bytearray(4096)

    src_region = eng.register_memory(src)
    dst_region = eng.register_memory(dst)
    remote = peer.import_region(src_region.descriptor())

    req = eng.read(peer, dst_region, remote, length=4096)
    for _ in range(100):
        eng.poll()
        if req.done:
            break
    check(req.wait(1000) == "ok", "read completes")
    check(bytes(dst) == bytes(src), "bytes arrive intact")
    check(req.reached("target_ready"), "reaches target_ready")


def test_registration_keeps_its_buffer_alive():
    """The one with no Python-level symptom.

    A caller registers a buffer and stops referencing it. Nothing in the
    object model would keep it alive, and the NIC would be left pointing at
    freed memory -- a crash with no traceback, long after the line that
    caused it.
    """
    eng, peer = setup()
    payload = bytes(bytearray(range(256)) * 16)

    src = bytearray(payload)
    src_region = eng.register_memory(src)
    remote = peer.import_region(src_region.descriptor())

    del src
    gc.collect()  # the only reference left is the one the engine holds

    dst = bytearray(len(payload))
    dst_region = eng.register_memory(dst)
    req = eng.read(peer, dst_region, remote, length=len(payload))
    for _ in range(100):
        eng.poll()
        if req.done:
            break
    check(req.wait(1000) == "ok", "transfer survives the caller dropping it")
    check(bytes(dst) == payload, "contents are still correct afterwards")


def test_blocking_wait_releases_the_gil():
    """A wait that held the GIL would freeze every other thread, including
    whichever one would have polled for the completion it is waiting on.

    Only ticks landing inside the wait count. Counting everything the other
    thread did would include what it managed before and after, which is enough
    to hide the difference entirely -- the first version of this test passed
    either way.
    """
    eng, peer = setup(move_data=False)
    buf = bytearray(4096)
    region = eng.register_memory(buf)
    remote = peer.import_region(region.descriptor())
    req = eng.read(peer, region, remote, length=4096)

    stamps = []
    stop = [False]

    def tick():
        while not stop[0]:
            stamps.append(time.time())
            time.sleep(0.005)

    t = threading.Thread(target=tick)
    t.start()
    time.sleep(0.05)  # let the thread reach a steady rhythm first

    start = time.time()
    req.wait(300)
    end = time.time()

    stop[0] = True
    t.join()

    during = [s for s in stamps if start < s < end]
    # Holding the GIL yields about one; releasing it, dozens.
    check(len(during) > 10,
          f"other threads run during the wait ({len(during)} ticks inside it)")


def test_batch_submits_once():
    eng, peer = setup()
    src = bytearray(bytes(range(256)) * 64)
    dst = bytearray(len(src))
    src_region = eng.register_memory(src)
    dst_region = eng.register_memory(dst)
    remote = peer.import_region(src_region.descriptor())

    offsets = [0, 4096, 8192]
    lengths = [1024, 1024, 1024]
    req = eng.readv(peer, dst_region, remote, offsets, offsets, lengths)
    for _ in range(200):
        eng.poll()
        if req.done:
            break
    check(req.wait(1000) == "ok", "readv completes")
    for off, ln in zip(offsets, lengths):
        if bytes(dst[off:off + ln]) != bytes(src[off:off + ln]):
            check(False, f"segment at {off} matches")
            break
    else:
        check(True, "every segment matches")


def test_errors_are_exceptions_not_codes():
    eng, _ = setup()
    try:
        eng.register_memory(42)  # not a buffer
        check(False, "registering a non-buffer raises")
    except Exception:
        check(True, "registering a non-buffer raises")

    buf = bytearray(1024)
    region = eng.register_memory(buf)
    try:
        # Past the end of the region.
        peer = eng.add_peer(eng.local_metadata())
        remote = peer.import_region(region.descriptor())
        eng.read(peer, region, remote, local_offset=0, remote_offset=0,
                 length=1 << 20)
        check(False, "an out-of-range transfer raises")
    except Exception:
        check(True, "an out-of-range transfer raises")


def test_reports_what_it_ran():
    eng, _ = setup()
    desc = eng.describe()
    check('"engine"' in desc and '"provider"' in desc,
          "describe covers both halves")
    st = eng.stats()
    check("payload_bytes_copied" in st, "stats expose the copy counter")
    check(isinstance(st["requests_accepted"], int), "stats are plain numbers")


def test_peer_says_which_path_it_got():
    """A fallback to a slower transport should be a question you can ask,
    not something you infer from throughput."""
    eng, _ = setup()
    peer = eng.add_peer(eng.local_metadata())
    caps = peer.caps()
    check(caps["provider"] == "mock", "the provider is named")
    # Same process, but the only transport here is the network-shaped mock,
    # so the path must not claim to be local.
    check(caps["place"] == "same_process", "where the peer is")
    check(caps["path"] == "rdma", "how it is actually reached")


def test_deregistering_lets_the_registration_go():
    """Dropping the Python object is not enough: the engine holds the region
    too, and the reuse cache holds the registration after that."""
    eng, _ = setup()
    buf = bytearray(8192)
    region = eng.register_memory(buf)
    check(eng.release_cached_registrations() == 0,
          "nothing is released while the region is live")
    eng.deregister_memory(region)
    check(eng.release_cached_registrations() == 1,
          "released once the region is retired")


def test_a_retired_region_refuses_rather_than_crashes():
    eng, _ = setup()
    region = eng.register_memory(bytearray(4096))
    eng.deregister_memory(region)
    try:
        region.descriptor()
        check(False, "exporting a retired region raises")
    except RuntimeError:
        check(True, "exporting a retired region raises")


def main():
    for fn in [
        test_round_trip,
        test_registration_keeps_its_buffer_alive,
        test_blocking_wait_releases_the_gil,
        test_batch_submits_once,
        test_errors_are_exceptions_not_codes,
        test_reports_what_it_ran,
        test_peer_says_which_path_it_got,
        test_deregistering_lets_the_registration_go,
        test_a_retired_region_refuses_rather_than_crashes,
    ]:
        print(f"{fn.__name__}:")
        fn()

    print()
    if FAILURES:
        print(f"{len(FAILURES)} failed")
        return 1
    print("all python binding tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
