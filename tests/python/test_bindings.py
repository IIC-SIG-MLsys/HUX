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


def test_batch_registration_keeps_the_order():
    eng, _ = setup()
    bufs = [bytearray(4096), bytearray(8192), bytearray(2048)]
    regs = eng.register_memory_batch(bufs)
    check(len(regs) == 3, "one result per buffer")
    check(all(r is not None for r in regs), "all three registered")
    # The handles are independent: retiring one leaves the others usable.
    eng.deregister_memory(regs[0])
    check(regs[1].descriptor() is not None, "the others still export")


def test_a_notification_comes_back_with_its_payload():
    """Sending succeeds as soon as the bytes leave, which says nothing about
    the peer having them. What the request waits on is the acknowledgement."""
    eng, _ = setup()
    peer = eng.add_peer(eng.local_metadata())
    req = eng.notify(peer, b"hello from python")
    eng.progress()
    notes = eng.poll_notifications()
    check(len(notes) == 1, "one notification arrived")
    check(notes[0]["payload"] == b"hello from python", "payload survived")
    check(isinstance(notes[0]["id"], int), "it carries an id")


def test_removing_a_peer_disconnects_it():
    eng, _ = setup()
    peer = eng.add_peer(eng.local_metadata())
    check(peer.connected, "connected to begin with")
    eng.remove_peer(peer)
    check(not peer.connected, "not connected afterwards")


def _cuda_stream():
    """A stream from outside this library, the way a framework hands one over.

    ctypes rather than torch, so the test needs no framework installed: what
    is being checked is that a native handle crosses the boundary, and where
    the integer came from is exactly what the adapter is not supposed to
    care about.
    """
    import ctypes

    try:
        cudart = ctypes.CDLL("libcudart.so")
    except OSError:
        return None, None
    handle = ctypes.c_void_p()
    if cudart.cudaSetDevice(0) != 0:
        return None, None
    if cudart.cudaStreamCreate(ctypes.byref(handle)) != 0:
        return None, None
    return cudart, handle.value


def test_a_stream_the_caller_owns_can_be_adapted():
    if not hux.device_backend_available():
        print("  skip  built without a device backend")
        return
    cudart, native = _cuda_stream()
    if native is None:
        print("  skip  no usable device on this machine")
        return

    eng = hux.make_mock_engine(gpu=0)
    check(eng.has_device(), "the engine has a device backend")
    stream = eng.import_stream(native)
    check(stream is not None, "a native handle becomes a stream")
    event = eng.record_event(stream)
    check(event is not None, "a point in it becomes an event")
    eng.stream_wait_event(stream, event)
    check(True, "and later work can be made to wait for it")


def test_a_transfer_can_be_ordered_after_an_event():
    """The point of the adapter: issue a transfer whose source a kernel is
    still writing, and have the adapter wait rather than the caller."""
    if not hux.device_backend_available():
        print("  skip  built without a device backend")
        return
    cudart, native = _cuda_stream()
    if native is None:
        print("  skip  no usable device on this machine")
        return

    eng = hux.make_mock_engine(gpu=0)
    stream = eng.import_stream(native)
    event = eng.record_event(stream)

    buf = bytearray(4096)
    reg = eng.register_memory(buf)
    peer = eng.add_peer(eng.local_metadata())
    remote = peer.import_region(reg.descriptor())

    req = eng.write(peer, reg, remote, 0, 0, 4096, after=[event])
    for _ in range(200):
        eng.poll()
        if req.state in ("succeeded", "failed", "cancelled"):
            break
    check(req.state == "succeeded", "a transfer gated on an event completes")

    batched = eng.writev(peer, reg, remote, [0], [0], [4096], after=[event])
    for _ in range(200):
        eng.poll()
        if batched.state in ("succeeded", "failed", "cancelled"):
            break
    check(batched.state == "succeeded", "and so does a batch")


def test_the_stream_calls_refuse_without_a_backend():
    """Rather than crashing, which is what dereferencing the absent backend
    would do."""
    if hux.device_backend_available():
        print("  skip  this build has a backend")
        return
    eng = hux.make_mock_engine()
    check(not eng.has_device(), "the engine says it has no device")
    try:
        eng.import_stream(0)
        check(False, "import_stream raises")
    except RuntimeError:
        check(True, "import_stream raises")


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
        test_batch_registration_keeps_the_order,
        test_a_notification_comes_back_with_its_payload,
        test_removing_a_peer_disconnects_it,
        test_a_stream_the_caller_owns_can_be_adapted,
        test_a_transfer_can_be_ordered_after_an_event,
        test_the_stream_calls_refuse_without_a_backend,
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
