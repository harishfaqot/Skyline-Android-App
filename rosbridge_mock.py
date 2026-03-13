#!/usr/bin/env python3
"""
rosbridge_mock.py  —  Optional test mock for Skyline GCS
=========================================================

On the real robot you do NOT need this script at all.
Just run rosbridge normally:

    roslaunch rosbridge_server rosbridge_websocket.launch

Skyline will connect to  ws://<ROBOT_IP>:9090  and subscribe to:
    /cloud_registered   (sensor_msgs/PointCloud2)
    /image_raw          (sensor_msgs/Image)
    /path               (nav_msgs/Path)
    /Odometry           (nav_msgs/Odometry)
    /telemetry          (custom JSON)

────────────────────────────────────────────────────────────
DESKTOP MOCK  (no ROS needed – for UI development / testing)
────────────────────────────────────────────────────────────
This script implements a minimal rosbridge server that publishes
fake data so you can verify the Skyline UI without a robot.

Requirements:
    pip install websockets numpy opencv-python pillow

Usage:
    python3 rosbridge_mock.py
    # Then connect Skyline to ws://localhost:9090
"""

import asyncio
import json
import math
import time
import struct
import base64
import io
import numpy as np

try:
    import websockets
    from websockets.server import serve
except ImportError:
    print("pip install websockets")
    raise

try:
    from PIL import Image as PILImage
except ImportError:
    PILImage = None


HOST = "0.0.0.0"
PORT = 9090


class RosbridgeMock:
    def __init__(self):
        self.clients: set = set()
        self.t = 0.0
        self.path_history = []
        self.max_path_points = None

    # ── helpers ──────────────────────────────────────────────────────────────

    def _publish(self, topic: str, msg_type: str, msg: dict) -> str:
        return json.dumps({"op": "publish", "topic": topic, "type": msg_type, "msg": msg})

    def _header(self, seq=0, frame_id="map"):
        return {
            "seq": seq,
            "stamp": {"secs": int(self.t), "nsecs": int((self.t % 1) * 1e9)},
            "frame_id": frame_id
        }

    # ── message generators ───────────────────────────────────────────────────

    def make_pointcloud2(self):
        """Random 500-point cloud as sensor_msgs/PointCloud2 (rgb8 packed float32 xyz)"""
        n = 500
        xs = np.random.uniform(-10, 10, n).astype(np.float32)
        ys = np.random.uniform(-10, 10, n).astype(np.float32)
        zs = np.random.uniform(-2, 6, n).astype(np.float32)
        point_step = 12  # 3 × float32
        buf = bytearray(n * point_step)
        for i, (x, y, z) in enumerate(zip(xs, ys, zs)):
            struct.pack_into('fff', buf, i * point_step, x, y, z)
        b64 = base64.b64encode(buf).decode()
        msg = {
            "header": self._header(frame_id="map"),
            "height": 1,
            "width": n,
            "fields": [
                {"name": "x", "offset": 0, "datatype": 7, "count": 1},
                {"name": "y", "offset": 4, "datatype": 7, "count": 1},
                {"name": "z", "offset": 8, "datatype": 7, "count": 1},
            ],
            "is_bigendian": False,
            "point_step": point_step,
            "row_step": n * point_step,
            "data": b64,
            "is_dense": True,
        }
        return self._publish("/cloud_registered", "sensor_msgs/PointCloud2", msg)

    def make_image(self):
        """Simple colour gradient as sensor_msgs/Image rgb8 320×240"""
        w, h = 320, 240
        img_array = np.zeros((h, w, 3), dtype=np.uint8)
        t = self.t
        for row in range(h):
            r = int(128 + 127 * math.sin(t + row / 30.0))
            g = int(128 + 127 * math.cos(t + row / 40.0))
            b = 100
            img_array[row, :] = [r, g, b]
        raw = img_array.tobytes()
        b64 = base64.b64encode(raw).decode()
        msg = {
            "header": self._header(frame_id="camera"),
            "height": h,
            "width":  w,
            "encoding": "rgb8",
            "is_bigendian": 0,
            "step": w * 3,
            "data": b64,
        }
        return self._publish("/image_raw", "sensor_msgs/Image", msg)

    def make_path(self):
        """Path generated from recent odometry positions."""
        poses = []
        for x, y, z in self.path_history:
            poses.append({
                "header": self._header(frame_id="map"),
                "pose": {
                    "position": {"x": x, "y": y, "z": z},
                    "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0}
                }
            })
        msg = {"header": self._header(frame_id="map"), "poses": poses}
        return self._publish("/path", "nav_msgs/Path", msg)

    def make_odometry(self):
        """Helical drone pose (circle + increasing z) as nav_msgs/Odometry"""
        radius = 5.0
        px = radius * math.cos(self.t * 0.3)
        py = radius * math.sin(self.t * 0.3)
        pz = 0.5 + 0.03 * self.t
        yaw = self.t * 0.3
        qz = math.sin(yaw / 2)
        qw = math.cos(yaw / 2)

        self.path_history.append((px, py, pz))
        if self.max_path_points is not None and len(self.path_history) > self.max_path_points:
            self.path_history = self.path_history[-self.max_path_points:]

        msg = {
            "header": self._header(frame_id="map"),
            "child_frame_id": "base_link",
            "pose": {
                "pose": {
                    "position": {"x": px, "y": py, "z": pz},
                    "orientation": {"x": 0.0, "y": 0.0, "z": qz, "w": qw}
                },
                "covariance": [0.0] * 36
            },
            "twist": {
                "twist": {
                    "linear":  {"x": 0.0, "y": 0.0, "z": 0.0},
                    "angular": {"x": 0.0, "y": 0.0, "z": 0.0}
                },
                "covariance": [0.0] * 36
            }
        }
        return self._publish("/Odometry", "nav_msgs/Odometry", msg)

    def make_telemetry(self):
        """Simple telemetry packet with battery, signal, and flight mode."""
        battery = max(20, 100 - int(self.t * 0.5))
        signal = int(70 + 25 * math.sin(self.t * 0.2))
        modes = ["MANUAL", "AUTO", "GPS", "LOITER"]
        mode = modes[int(self.t / 20) % len(modes)]

        msg = {
            "battery": int(np.clip(battery, 0, 100)),
            "signal": int(np.clip(signal, 0, 100)),
            "mode": mode,
        }
        return self._publish("/telemetry", "std_msgs/String", msg)

    # ── server ───────────────────────────────────────────────────────────────

    async def handle_client(self, websocket, path=None):
        self.clients.add(websocket)
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
                        print(f"    subscribe → {topic}")
                    elif op == "advertise":
                        print(f"    advertise → {topic}")
                except Exception:
                    pass
        except Exception:
            pass
        finally:
            self.clients.discard(websocket)
            print(f"[-] Client disconnected: {addr}")

    async def broadcast(self, message: str):
        dead = set()
        for ws in self.clients:
            try:
                await ws.send(message)
            except Exception:
                dead.add(ws)
        for ws in dead:
            self.clients.discard(ws)

    async def publish_loop(self):
        frame = 0
        fps = 10
        print("Mock publishing started at ~10 Hz")
        while True:
            self.t += 1.0 / fps

            await self.broadcast(self.make_odometry())

            if frame % 3 == 0:
                await self.broadcast(self.make_path())

            if frame % 5 == 0:
                await self.broadcast(self.make_pointcloud2())

            if frame % 2 == 0:
                await self.broadcast(self.make_image())

            if frame % 2 == 0:
                await self.broadcast(self.make_telemetry())

            frame += 1
            await asyncio.sleep(1.0 / fps)

    async def start(self):
        print("=" * 50)
        print("rosbridge MOCK server")
        print(f"ws://{HOST}:{PORT}")
        print("Topics: /cloud_registered /image_raw /path /Odometry /telemetry")
        print("=" * 50)
        async with serve(self.handle_client, HOST, PORT):
            await self.publish_loop()


if __name__ == "__main__":
    mock = RosbridgeMock()
    try:
        asyncio.run(mock.start())
    except KeyboardInterrupt:
        print("Stopped.")
