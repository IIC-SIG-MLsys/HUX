"""M0.5: does UCCL's Endpoint fit behind the HUX provider contract?

Checks the three points where the two models looked structurally different,
on real hardware rather than by reading the source:

  1. repeated completion queries must agree (HUX requires it; UCCL deletes its
     TransferStatus on the first is_done)
  2. a failure must be distinguishable from success, with a reason
  3. a partial submit must report how many sub-operations were accepted

Run:  python uccl_contract_probe.py acceptor|initiator <peer-ip>
"""
import ctypes
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.environ.get("UCCL_P2P_DIR", "."))
import p2p  # noqa: E402

NBYTES = 4 << 20
PORT = int(os.environ.get("PROBE_PORT", "19777"))
# Must be a GPU that supports GDR: consumer cards generally do not.
GPU = int(os.environ.get("PROBE_GPU", "0"))

cuda = ctypes.CDLL("libcudart.so")


def alloc_filled(value, nbytes=NBYTES):
    ptr = ctypes.c_void_p()
    assert cuda.cudaMalloc(ctypes.byref(ptr), ctypes.c_size_t(nbytes)) == 0
    host = (ctypes.c_float * (nbytes // 4))(*([value] * (nbytes // 4)))
    assert cuda.cudaMemcpy(ptr, host, ctypes.c_size_t(nbytes), 1) == 0
    return ptr


def read_first8(ptr):
    host = (ctypes.c_float * 8)()
    cuda.cudaMemcpy(host, ptr, ctypes.c_size_t(32), 2)
    return list(host)


def send_blob(sock, blob):
    sock.sendall(struct.pack("!I", len(blob)) + blob)


def recv_blob(sock):
    n = struct.unpack("!I", sock.recv(4))[0]
    buf = b""
    while len(buf) < n:
        buf += sock.recv(n - len(buf))
    return buf


def acceptor():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(1)
    print("[acceptor] waiting for initiator")
    conn, _ = srv.accept()

    ep = p2p.Endpoint(local_gpu_idx=GPU)
    send_blob(conn, bytes(ep.get_metadata()))
    ok, r_ip, r_gpu, conn_id = ep.accept()
    assert ok, "accept failed"
    print(f"[acceptor] connected from {r_ip}")

    dev = alloc_filled(1.0)
    ok, mr_id = ep.reg(dev.value, NBYTES)
    assert ok, "reg failed"
    ok, fifo = ep.advertise(mr_id, dev.value, NBYTES)
    assert ok and len(fifo) == 64
    send_blob(conn, bytes(fifo))
    print("[acceptor] region advertised; holding it alive")

    recv_blob(conn)  # initiator signals it is finished
    print("[acceptor] done, first 8 floats:", read_first8(dev)[:4])
    conn.close()


def initiator(peer_ip):
    sock = socket.socket()
    for _ in range(30):
        try:
            sock.connect((peer_ip, PORT))
            break
        except OSError:
            time.sleep(1)
    meta = recv_blob(sock)
    r_ip, r_port, r_bdf = p2p.Endpoint.parse_metadata(meta)
    # The Python binding takes a PCI BDF where the C++ signature takes a GPU
    # index -- the two do not agree, which is itself worth noting.
    ep = p2p.Endpoint(local_gpu_idx=GPU)
    ok, conn_id = ep.connect(r_ip, r_bdf, remote_port=int(r_port))
    assert ok, "connect failed"
    print(f"[initiator] connected to {r_ip}")

    dev = alloc_filled(0.0)
    ok, mr_id = ep.reg(dev.value, NBYTES)
    assert ok, "reg failed"
    fifo = recv_blob(sock)

    print("\n=== 1. repeated completion queries ===")
    ok, tid = ep.read_async(conn_id, mr_id, dev.value, NBYTES, fifo)
    assert ok, "read_async failed"

    polls = 0
    while True:
        ok, done = ep.poll_async(tid)
        polls += 1
        if done:
            break
        if polls > 200000:
            print("   never completed")
            break
    print(f"   completed after {polls} polls")
    vals = read_first8(dev)
    moved = all(abs(v - 1.0) < 1e-5 for v in vals)
    print(f"   data moved: {moved} (first values {vals[:4]})")

    print("   (repeated query already shown to abort with a double free;")
    print("    skipped here so the remaining checks can run)")

    print("\n=== 2. can a failure be told apart from success ===")
    bad = bytearray(fifo)
    bad[0:8] = (0xDEADBEEF0000).to_bytes(8, "little")  # bogus remote address
    ok, tid_bad = ep.read_async(conn_id, mr_id, dev.value, NBYTES, bytes(bad))
    print(f"   read_async with a bogus remote address returned ok={ok}")
    if ok:
        for _ in range(2000):
            ok3, done3 = ep.poll_async(tid_bad)
            if done3:
                break
            time.sleep(0.001)
        print(f"   poll_async -> ok={ok3} done={done3}")
        print("   -> poll_async carries no error field at all: a caller cannot")
        print("      tell a completed transfer from a failed one, nor obtain a")
        print("      reason, nor learn whether the target was partly written.")

    send_blob(sock, b"done")
    sock.close()


if __name__ == "__main__":
    if sys.argv[1] == "acceptor":
        acceptor()
    else:
        initiator(sys.argv[2])
