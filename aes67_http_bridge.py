#!/usr/bin/env python3
"""
AES67 → HTTP PCM bridge (ultra low latency, multi-client)

- Joins an RTP multicast group carrying L24/48000/2 (e.g., Shure ANI22).
- Depayloads RTP (PT from SDP, default 97) and converts 24-bit BE to 16-bit LE.
- Serves a continuous HTTP stream at /audio using chunked transfer (application/octet-stream).
- Fan-out to multiple clients with tiny per-client queues; drops oldest chunks to keep latency tiny.
- Clean reconnects (no noisy tracebacks when clients come/go).

Quick start (example from the question):
    python3 aes67_http_bridge.py \
        --mcast 239.168.227.217 --port 5004 --iface-ip 192.168.25.101 \
        --http 0.0.0.0 --http-port 53354

Windows playback (very low latency):
    ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 \
        -f s16le -ac 2 -ar 48000 http://<UBUNTU_IP>:53354/audio

Dependencies:
    pip install aiohttp
"""
import argparse
import asyncio
import socket
import struct
import time
from typing import Dict, Optional
from aiohttp import web

# ---------- RTP parsing ----------
def parse_rtp(packet: bytes):
    """Return (payload_type, timestamp, payload_bytes) or (None, None, None)."""
    if len(packet) < 12:
        return None, None, None
    b0, b1, seq, ts, ssrc = struct.unpack('!BBHII', packet[:12])
    v = (b0 >> 6) & 0x03
    if v != 2:
        return None, None, None
    cc = b0 & 0x0F
    x = (b0 >> 4) & 0x01
    pt = b1 & 0x7F
    header_len = 12 + cc * 4
    if len(packet) < header_len:
        return None, None, None
    if x:
        if len(packet) < header_len + 4:
            return None, None, None
        _, ext_len = struct.unpack('!HH', packet[header_len:header_len+4])
        header_len += 4 + ext_len * 4
        if len(packet) < header_len:
            return None, None, None
    return pt, ts, packet[header_len:]

def l24be_stereo_to_s16le(buf: bytes) -> bytes:
    """Convert interleaved L24 BE stereo → S16 LE stereo by dropping LSB."""
    # Stereo frame = 6 bytes (L:3, R:3). Output frame = 4 bytes (L:2, R:2).
    out = bytearray((len(buf) // 6) * 4)
    o = 0
    for i in range(0, len(buf) - 5, 6):
        # Take the top 16 bits of each 24-bit BE sample and swap for LE.
        L0, L1 = buf[i], buf[i+1]
        R0, R1 = buf[i+3], buf[i+4]
        out[o]   = L1; out[o+1] = L0
        out[o+2] = R1; out[o+3] = R0
        o += 4
    if o != len(out):
        del out[o:]  # trim if partial frame at end
    return bytes(out)

# ---------- RTP receiver ----------
class RTPReceiver:
    def __init__(self, mcast: str, port: int, iface_ip: str, payload_type: int = 97, recv_buf: int = 262144):
        self.mcast = mcast
        self.port = port
        self.iface_ip = iface_ip
        self.payload_type = payload_type
        self.recv_buf = recv_buf
        self.sock: Optional[socket.socket] = None
        self.running = False
        self.out_queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=128)  # small, but enough for bursts

    def open_socket(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, self.recv_buf)
        except OSError:
            pass
        try:
            s.bind(('', self.port))
        except OSError:
            # Some stacks require group bind
            s.bind((self.mcast, self.port))
        # Join multicast (source-agnostic)
        mreq = socket.inet_aton(self.mcast) + socket.inet_aton(self.iface_ip)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        # Prefer receiving via the desired interface
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(self.iface_ip))
        # Optional: don't loop back
        try:
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 0)
        except OSError:
            pass
        self.sock = s

    async def run(self):
        self.running = True
        self.open_socket()
        self.sock.setblocking(False)
        loop = asyncio.get_running_loop()
        print(f"[RTP] Listening {self.mcast}:{self.port} via {self.iface_ip}, PT={self.payload_type}")
        while self.running:
            try:
                data = await loop.sock_recv(self.sock, 2048)
            except (asyncio.CancelledError, OSError):
                break
            pt, ts, payload = parse_rtp(data)
            if pt is None or payload is None or pt != self.payload_type:
                continue
            pcm16 = l24be_stereo_to_s16le(payload)
            # Keep latency tiny: drop oldest chunk if we fall behind
            if self.out_queue.full():
                try:
                    self.out_queue.get_nowait()
                except asyncio.QueueEmpty:
                    pass
            await self.out_queue.put(pcm16)

    async def stop(self):
        self.running = False
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass

# ---------- Fan-out hub to multiple clients ----------
class FanOut:
    def __init__(self, receiver: RTPReceiver, client_queue_chunks: int = 64):
        self.receiver = receiver
        self.client_queues: Dict[int, asyncio.Queue[bytes]] = {}
        self.client_queue_chunks = client_queue_chunks
        self._task: Optional[asyncio.Task] = None
        self._next_id = 1
        self._lock = asyncio.Lock()

    async def start(self):
        self._task = asyncio.create_task(self._pump())

    async def stop(self):
        if self._task:
            self._task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._task

    async def _pump(self):
        while True:
            chunk = await self.receiver.out_queue.get()
            # Send to all clients, non-blocking; drop when their queue is full
            dead = []
            for cid, q in self.client_queues.items():
                if q.full():
                    # drop oldest to keep latency low
                    try:
                        q.get_nowait()
                    except asyncio.QueueEmpty:
                        pass
                try:
                    q.put_nowait(chunk)
                except asyncio.QueueFull:
                    # If still full, drop silently
                    pass
            # no explicit cleanup here; clients remove themselves on disconnect

    async def add_client(self) -> (int, asyncio.Queue):
        async with self._lock:
            cid = self._next_id
            self._next_id += 1
            q: asyncio.Queue[bytes] = asyncio.Queue(maxsize=self.client_queue_chunks)
            self.client_queues[cid] = q
            return cid, q

    async def remove_client(self, cid: int):
        async with self._lock:
            self.client_queues.pop(cid, None)

# ---------- HTTP app ----------
async def make_app(fanout: FanOut, http_path: str):
    routes = web.RouteTableDef()

    @routes.get(http_path)
    async def audio_stream(request):
        resp = web.StreamResponse(status=200, reason='OK', headers={
            'Content-Type': 'application/octet-stream',
            'Cache-Control': 'no-store',
            'Pragma': 'no-cache',
            'Connection': 'close',
        })
        await resp.prepare(request)
        cid, q = await fanout.add_client()
        peer = request.remote
        print(f"[HTTP] Client connected (id={cid}, from={peer})")
        try:
            while True:
                chunk = await q.get()
                try:
                    await resp.write(chunk)  # no drain(); write flushes
                except (ConnectionResetError, ConnectionError, asyncio.CancelledError, BrokenPipeError):
                    break
        finally:
            print(f"[HTTP] Client disconnected (id={cid})")
            await fanout.remove_client(cid)
            try:
                await resp.write_eof()
            except Exception:
                pass
        return resp

    @routes.get('/healthz')
    async def healthz(request):
        return web.json_response({'ok': True, 'clients': len(fanout.client_queues)})

    app = web.Application()
    app.add_routes(routes)
    return app

# ---------- Main ----------
async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mcast', default='239.168.227.217', help='Multicast group to join (ANI22 RTP)')
    ap.add_argument('--port', type=int, default=5004, help='UDP port for RTP')
    ap.add_argument('--iface-ip', required=True, help='Local interface IP for joining the group (e.g., 192.168.25.101)')
    ap.add_argument('--payload-type', type=int, default=97, help='RTP payload type (from SDP)')
    ap.add_argument('--http', default='0.0.0.0', help='HTTP listen address')
    ap.add_argument('--http-port', type=int, default=53354, help='HTTP listen port')
    ap.add_argument('--path', default='/audio', help='HTTP path for the audio stream')
    ap.add_argument('--client-queue', type=int, default=64, help='Chunks per client queue (lower = lower latency)')
    args = ap.parse_args()

    receiver = RTPReceiver(args.mcast, args.port, args.iface_ip, args.payload_type)
    rtp_task = asyncio.create_task(receiver.run())

    fanout = FanOut(receiver, client_queue_chunks=args.client_queue)
    await fanout.start()

    app = await make_app(fanout, args.path)
    runner = web.AppRunner(app, access_log=None)  # quieter logs
    await runner.setup()
    # site = web.TCPSite(runner, args.http, args.http_port, reuse_address=True, reuse_port=True)
    site = web.TCPSite(runner, args.http, args.http_port, reuse_address=True)

    await site.start()
    print(f"[HTTP] Serving PCM16LE @ 48kHz stereo on http://{args.http}:{args.http_port}{args.path}")
    print("      Windows: ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 "
          f"-f s16le -ac 2 -ar 48000 http://<UBUNTU_IP>:{args.http_port}{args.path}")

    try:
        await rtp_task
    finally:
        await receiver.stop()
        await runner.cleanup()

if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass

