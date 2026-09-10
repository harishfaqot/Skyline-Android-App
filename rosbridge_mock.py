#!/usr/bin/env python3
"""
rosbridge_mock.py — Desktop mock of the full Skyline downlink (no ROS/drone needed)
=====================================================================================

Fakes everything skyline_app.py + pointcloud_ws_server.py normally serve from
the Jetson, using the exact same ports/wire formats, so the Qt app can't
tell the difference:

    ws://<HOST>:9090   rosbridge-style JSON — /path, /Odometry
                        (see RosPointCloudClient.h / skyline_app.py)
    ws://<HOST>:9092   binary point-cloud frames
                        (see PointCloudClient.h / pointcloud_ws_server.py)
    ws://<HOST>:9093   raw JSON telemetry, PLUS the app's REC-button command
                        uplink ({"cmd":"set_recording","enabled":...})
                        (see TelemetryClient.h / skyline_app.py)

Video (H.265/RTSP on :8554) is NOT mocked here — RtspVideoClient talks raw
GStreamer to a real RTSP server (see video_rtsp_server.py), and faking that
needs an actual GStreamer install, not something `pip install` gets you.
Point Skyline at this mock and the camera tile just stays disconnected;
everything else — map, flight path, battery/signal/mode, ArduPilot
messages, and the REC button — works end to end.

Requirements:
    pip install websockets numpy

Usage:
    python3 rosbridge_mock.py
    # Then connect Skyline to ws://<this machine's IP>:9090
    # (it derives :9092/:9093 from that same host automatically)
"""

import asyncio
import json
import math
import random
import struct
import time

import numpy as np

try:
    from websockets.asyncio.server import serve
except ImportError:
    print("pip install websockets")
    raise


HOST = "0.0.0.0"
PORT = 9090          # /path, /Odometry (rosbridge-style JSON)
POINTCLOUD_PORT = 9092  # binary point-cloud frames
TELEMETRY_PORT = 9093   # raw JSON telemetry + recording command uplink

PUBLISH_HZ = 10.0
CLOUD_HZ = 3.0          # matches pointcloud_ws_server.py's CLOUD_MAX_HZ
CLOUD_SAMPLE_POINTS = 350  # points sent per frame, well under CLOUD_MAX_POINTS=600
TELEMETRY_HZ = 2.0       # matches skyline_app.py's TELEMETRY_HZ

# ── point-cloud binary wire format — MUST match pointcloud_ws_server.py ────
# little-endian: uint8 version, uint32 seq, uint32 stamp_secs,
# uint32 stamp_nsecs, uint32 num_points, then float32[num_points*3] xyz
HEADER = struct.Struct("<BIIII")
WIRE_VERSION = 1

# ── battery percent from voltage (6S) — mirrors skyline_app.py exactly, so
# the mock exercises the same code path/thresholds the real bridge uses ──
BATTERY_CELLS = 6
_LIPO_CURVE = [
    (4.20, 100), (4.15, 95), (4.11, 90), (4.08, 85), (4.02, 80),
    (3.98, 75), (3.95, 70), (3.91, 65), (3.87, 60), (3.85, 55),
    (3.84, 50), (3.82, 45), (3.80, 40), (3.79, 35), (3.77, 30),
    (3.75, 25), (3.73, 20), (3.71, 15), (3.69, 10), (3.61, 5),
    (3.27, 0),
]


def voltage_to_percent(pack_voltage, cells=BATTERY_CELLS):
    cell_v = pack_voltage / cells
    if cell_v >= _LIPO_CURVE[0][0]:
        return 100.0
    if cell_v <= _LIPO_CURVE[-1][0]:
        return 0.0
    for (v_hi, p_hi), (v_lo, p_lo) in zip(_LIPO_CURVE, _LIPO_CURVE[1:]):
        if v_lo <= cell_v <= v_hi:
            frac = (cell_v - v_lo) / (v_hi - v_lo)
            return round(p_lo + frac * (p_hi - p_lo), 1)
    return None


# A grab-bag of realistic ArduPilot STATUSTEXT lines, tagged with the
# MAV_SEVERITY they'd actually carry, so the app's Messages panel / toast
# behavior can be exercised without a real flight controller.
FAKE_STATUSTEXTS = [
    (6, "GPS: detected as u-blox"),
    (6, "EKF3 IMU0 origin set"),
    (6, "RC Channels: valid"),
    (5, "ArduCopter V4.5.0"),
    (4, "EKF3 IMU1 MAG0 ground mag anomaly"),
    (4, "Vibration compensation ON"),
    (4, "PreArm: Compass not calibrated"),
    (3, "PreArm: GPS 1: Bad fix"),
    (3, "Failsafe: RC signal lost"),
    (2, "EKF3 IMU0 forced reset"),
]


class RosbridgeMock:
    def __init__(self):
        # /path + /Odometry (port 9090)
        self.clients: set = set()
        self.client_subscriptions: dict = {}

        # point cloud (port 9092) — no subscribe handshake, matches
        # pointcloud_ws_server.py: every connected client gets every frame.
        self.cloud_clients: set = set()

        # telemetry (port 9093) — no subscribe handshake either, but IS
        # bidirectional (see handle_telemetry_client).
        self.telemetry_clients: set = set()

        self.t = 0.0
        self.seq = 0
        self.path_history = []
        self.max_path_points = 3000

        # Simulated flight/battery/recording state, driven purely by time
        # and by the app's own REC button — no ROS underneath any of it.
        self.armed = False
        self.recording = False
        self.pack_voltage = 25.2  # full 6S pack

        # A static synthetic "room" the drone appears to be mapping —
        # persistent so repeated frames build up a real-looking map in the
        # app instead of flickering random noise every tick.
        self.room_points = self._make_room()

        self._pending_messages = []
        self._last_message_at = 0.0

    # ── synthetic environment ────────────────────────────────────────────

    @staticmethod
    def _make_room():
        pts = []
        # Floor scatter
        for _ in range(1200):
            pts.append((random.uniform(-8, 8), random.uniform(-8, 8), random.uniform(-0.1, 0.1)))
        # Four walls
        for _ in range(900):
            side = random.randint(0, 3)
            if side == 0:
                pts.append((-8, random.uniform(-8, 8), random.uniform(0, 4)))
            elif side == 1:
                pts.append((8, random.uniform(-8, 8), random.uniform(0, 4)))
            elif side == 2:
                pts.append((random.uniform(-8, 8), -8, random.uniform(0, 4)))
            else:
                pts.append((random.uniform(-8, 8), 8, random.uniform(0, 4)))
        # A couple of box "obstacles" to make the map recognizable
        for cx, cy in [(3, -2), (-4, 4)]:
            for _ in range(200):
                pts.append((cx + random.uniform(-0.8, 0.8), cy + random.uniform(-0.8, 0.8), random.uniform(0, 1.5)))
        return pts

    # ── helpers ──────────────────────────────────────────────────────────

    def _publish(self, topic: str, msg_type: str, msg: dict) -> str:
        return json.dumps({"op": "publish", "topic": topic, "type": msg_type, "msg": msg})

    def _subscribed_clients(self, topic: str):
        return [ws for ws in self.clients if topic in self.client_subscriptions.get(ws, set())]

    def _header(self, seq=0, frame_id="map"):
        return {
            "seq": seq,
            "stamp": {"secs": int(self.t), "nsecs": int((self.t % 1) * 1e9)},
            "frame_id": frame_id,
        }

    # ── /path, /Odometry (port 9090) ────────────────────────────────────

    def make_path(self):
        poses = [
            {
                "header": self._header(frame_id="map"),
                "pose": {
                    "position": {"x": x, "y": y, "z": z},
                    "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
                },
            }
            for x, y, z in self.path_history
        ]
        msg = {"header": self._header(frame_id="map"), "poses": poses}
        return self._publish("/path", "nav_msgs/Path", msg)

    def _drone_pose(self):
        """Helical flight path (circle + slowly climbing)."""
        radius = 5.0
        px = radius * math.cos(self.t * 0.3)
        py = radius * math.sin(self.t * 0.3)
        pz = 0.5 + 0.03 * self.t
        yaw = self.t * 0.3
        return px, py, pz, yaw

    def make_odometry(self):
        px, py, pz, yaw = self._drone_pose()
        qz = math.sin(yaw / 2)
        qw = math.cos(yaw / 2)

        self.path_history.append((px, py, pz))
        if len(self.path_history) > self.max_path_points:
            self.path_history = self.path_history[-self.max_path_points:]

        msg = {
            "header": self._header(frame_id="map"),
            "child_frame_id": "base_link",
            "pose": {
                "pose": {
                    "position": {"x": px, "y": py, "z": pz},
                    "orientation": {"x": 0.0, "y": 0.0, "z": qz, "w": qw},
                },
                "covariance": [0.0] * 36,
            },
            "twist": {
                "twist": {
                    "linear": {"x": 0.0, "y": 0.0, "z": 0.0},
                    "angular": {"x": 0.0, "y": 0.0, "z": 0.0},
                },
                "covariance": [0.0] * 36,
            },
        }
        return self._publish("/Odometry", "nav_msgs/Odometry", msg)

    # ── point cloud (port 9092, binary) ─────────────────────────────────

    def make_pointcloud_frame(self) -> bytes:
        sample = random.sample(self.room_points, min(CLOUD_SAMPLE_POINTS, len(self.room_points)))
        xyz = np.asarray(sample, dtype="<f4")

        self.seq += 1
        now = time.time()
        header = HEADER.pack(
            WIRE_VERSION,
            self.seq & 0xFFFFFFFF,
            int(now) & 0xFFFFFFFF,
            int((now % 1) * 1e9) & 0xFFFFFFFF,
            len(xyz),
        )
        return header + xyz.tobytes()

    # ── telemetry (port 9093, raw JSON) ─────────────────────────────────

    def _maybe_queue_message(self):
        # Roughly one new ArduPilot message every ~8s, occasionally a
        # critical one, so the Messages panel / warning toast both get
        # exercised without needing to wait long.
        if self.t - self._last_message_at < 8.0:
            return
        self._last_message_at = self.t
        severity, text = random.choice(FAKE_STATUSTEXTS)
        self._pending_messages.append({"severity": severity, "text": text})

    def make_telemetry(self) -> str:
        # Slow discharge, wrapping around so a test session can watch the
        # battery pill cycle through green/yellow/red without restarting.
        self.pack_voltage -= 0.004
        if self.pack_voltage < 19.5:  # ~3.25V/cell, near _LIPO_CURVE's floor
            self.pack_voltage = 25.2

        battery = voltage_to_percent(self.pack_voltage)
        signal = int(np.clip(70 + 25 * math.sin(self.t * 0.2), 0, 100))
        modes = ["MANUAL", "STABILIZE", "AUTO", "GUIDED", "LOITER", "RTL"]
        mode = modes[int(self.t / 15) % len(modes)]
        self.armed = (int(self.t / 30) % 2) == 0

        px, py, pz, _ = self._drone_pose()

        self._maybe_queue_message()
        messages, self._pending_messages = self._pending_messages, []

        out = {
            "battery": int(round(battery)),
            "signal": signal,
            "mode": mode,
            "armed": self.armed,
            "connected": True,
            "voltage": round(self.pack_voltage, 2),
            "current": round(8.0 + 4.0 * math.sin(self.t * 0.5), 2),
            "latitude": -6.914744 + px * 1e-5,
            "longitude": 107.609810 + py * 1e-5,
            "altitude": round(pz, 2),
            "satellites": 14,
            "fix_type": 3,
            "recording": self.recording,
            "messages": messages,
        }
        return json.dumps(out)

    # ── /path+/Odometry server (port 9090) ──────────────────────────────

    async def handle_client(self, websocket):
        self.clients.add(websocket)
        self.client_subscriptions[websocket] = set()
        try:
            addr = f"{websocket.remote_address[0]}:{websocket.remote_address[1]}"
        except Exception:
            addr = "?"
        print(f"[+] Client connected: {addr}")

        try:
            async for raw in websocket:
                try:
                    msg = json.loads(raw)
                    op = msg.get("op", "")
                    topic = msg.get("topic", "")
                    if op == "subscribe":
                        self.client_subscriptions[websocket].add(topic)
                        print(f"    subscribe → {topic}")
                    elif op == "unsubscribe":
                        self.client_subscriptions[websocket].discard(topic)
                except Exception:
                    pass
        except Exception:
            pass
        finally:
            self.clients.discard(websocket)
            self.client_subscriptions.pop(websocket, None)
            print(f"[-] Client disconnected: {addr}")

    async def broadcast(self, message: str, topic: str):
        targets = self._subscribed_clients(topic)
        if not targets:
            return
        dead = set()
        for ws in targets:
            try:
                await ws.send(message)
            except Exception:
                dead.add(ws)
        for ws in dead:
            self.clients.discard(ws)
            self.client_subscriptions.pop(ws, None)

    # ── point-cloud server (port 9092) ──────────────────────────────────

    async def handle_cloud_client(self, websocket):
        self.cloud_clients.add(websocket)
        try:
            addr = f"{websocket.remote_address[0]}:{websocket.remote_address[1]}"
        except Exception:
            addr = "?"
        print(f"[+] Point-cloud client connected: {addr}")
        try:
            async for _ in websocket:
                pass  # no inbound protocol, matches pointcloud_ws_server.py
        except Exception:
            pass
        finally:
            self.cloud_clients.discard(websocket)
            print(f"[-] Point-cloud client disconnected: {addr}")

    async def broadcast_cloud(self, payload: bytes):
        if not self.cloud_clients:
            return
        dead = set()
        for ws in self.cloud_clients:
            try:
                await ws.send(payload)
            except Exception:
                dead.add(ws)
        self.cloud_clients -= dead

    # ── telemetry server (port 9093) ────────────────────────────────────

    async def handle_telemetry_client(self, websocket):
        self.telemetry_clients.add(websocket)
        try:
            addr = f"{websocket.remote_address[0]}:{websocket.remote_address[1]}"
        except Exception:
            addr = "?"
        print(f"[+] Telemetry client connected: {addr}")
        try:
            async for raw in websocket:
                # Mirrors skyline_app.py's handle_telemetry_client: the only
                # inbound message is the REC button's start/stop command.
                try:
                    cmd = json.loads(raw)
                    if cmd.get("cmd") == "set_recording":
                        self.recording = bool(cmd.get("enabled", False))
                        print(f"    set_recording({self.recording}) ← {addr}")
                except Exception:
                    pass
        except Exception:
            pass
        finally:
            self.telemetry_clients.discard(websocket)
            print(f"[-] Telemetry client disconnected: {addr}")

    async def broadcast_telemetry(self, message: str):
        if not self.telemetry_clients:
            return
        dead = set()
        for ws in self.telemetry_clients:
            try:
                await ws.send(message)
            except Exception:
                dead.add(ws)
        self.telemetry_clients -= dead

    # ── publish loops ────────────────────────────────────────────────────

    async def publish_loop(self):
        print(f"Mock /path + /Odometry started at {PUBLISH_HZ} Hz on :{PORT}")
        while True:
            self.t += 1.0 / PUBLISH_HZ
            await self.broadcast(self.make_odometry(), "/Odometry")
            await self.broadcast(self.make_path(), "/path")
            await asyncio.sleep(1.0 / PUBLISH_HZ)

    async def cloud_loop(self):
        print(f"Mock point cloud started at {CLOUD_HZ} Hz on :{POINTCLOUD_PORT}")
        while True:
            await self.broadcast_cloud(self.make_pointcloud_frame())
            await asyncio.sleep(1.0 / CLOUD_HZ)

    async def telemetry_loop(self):
        print(f"Mock telemetry started at {TELEMETRY_HZ} Hz on :{TELEMETRY_PORT}")
        while True:
            await self.broadcast_telemetry(self.make_telemetry())
            await asyncio.sleep(1.0 / TELEMETRY_HZ)

    async def start(self):
        print("=" * 60)
        print("rosbridge MOCK server (no ROS/drone needed)")
        print(f"ws://{HOST}:{PORT}              /path /Odometry")
        print(f"ws://{HOST}:{POINTCLOUD_PORT}              point cloud (binary)")
        print(f"ws://{HOST}:{TELEMETRY_PORT}              telemetry + REC command uplink")
        print("Video (:8554) is NOT mocked — see this file's module docstring")
        print("=" * 60)

        async with serve(
            self.handle_client, HOST, PORT,
            ping_interval=30, ping_timeout=30, close_timeout=10, max_queue=8, write_limit=2 ** 20,
        ), serve(
            self.handle_cloud_client, HOST, POINTCLOUD_PORT,
            ping_interval=30, ping_timeout=30, close_timeout=10, max_queue=8, write_limit=2 ** 20,
        ), serve(
            self.handle_telemetry_client, HOST, TELEMETRY_PORT,
            ping_interval=30, ping_timeout=30, close_timeout=10, max_queue=8, write_limit=2 ** 20,
        ):
            await asyncio.gather(self.publish_loop(), self.cloud_loop(), self.telemetry_loop())


if __name__ == "__main__":
    mock = RosbridgeMock()
    try:
        asyncio.run(mock.start())
    except KeyboardInterrupt:
        print("Stopped.")
