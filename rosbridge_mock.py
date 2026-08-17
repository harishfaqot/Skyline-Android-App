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
import struct
import base64
import numpy as np

try:
    from websockets.asyncio.server import serve
except ImportError:
    print("pip install websockets")
    raise

try:
    from PIL import Image as PILImage
except ImportError:
    PILImage = None

try:
    import cv2
except ImportError:
    cv2 = None


HOST = "0.0.0.0"
PORT = 9090

# Target camera publish rate. NOTE: 60 fps is only realistic with JPEG
# compression (see make_image below) — uncompressed rgb8 at 320x240 would
# need ~147 Mbps sustained at 60 fps, which no WiFi/emulator link will give
# you. With JPEG at IMAGE_JPEG_QUALITY below, a 320x240 frame is typically
# 8-20 KB instead of 300 KB, so 60 fps only needs ~4-10 Mbps.
IMAGE_FPS = 60
IMAGE_JPEG_QUALITY = 70  # 0-100, lower = smaller/faster, higher = better quality


class RosbridgeMock:
    def __init__(self):
        self.clients: set = set()
        self.client_subscriptions: dict = {}
        # Per-client "is a send still in flight for this topic" flag. Used to
        # drop a new frame instead of queuing it when a slow/throttled client
        # (e.g. an Android emulator's virtual NIC) hasn't finished receiving
        # the previous one yet. Without this, frames pile up on the wire and
        # you get an ever-growing multi-second backlog even though ping is
        # fine — ping only tests small ICMP packets, not sustained throughput.
        self.client_image_busy: dict = {}
        self.t = 0.0
        self.path_history = []
        self.max_path_points = None
        self.camera = None
        self.camera_ready = False
        self._camera_warning_printed = False
        self.preview_enabled = False
        self.preview_window_name = "Skyline Webcam Preview"
        self._preview_warning_printed = False

        if cv2 is None:
            print("[camera] OpenCV not installed, using synthetic image fallback")
            return

        # Try common backends (Windows first) and camera indices.
        backends = [cv2.CAP_DSHOW, cv2.CAP_MSMF, cv2.CAP_ANY]
        for cam_index in (0, 1):
            for backend in backends:
                cap = cv2.VideoCapture(cam_index, backend)
                if cap is not None and cap.isOpened():
                    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
                    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
                    self.camera = cap
                    self.camera_ready = True
                    self.preview_enabled = True
                    print(f"[camera] Using webcam index {cam_index} (backend={backend})")
                    print("[camera] Preview window enabled")
                    return
                if cap is not None:
                    cap.release()

        print("[camera] Could not open webcam, using synthetic image fallback")

    def _update_preview(self, frame_bgr):
        if not self.preview_enabled or cv2 is None:
            return

        try:
            cv2.imshow(self.preview_window_name, frame_bgr)
            cv2.waitKey(1)
        except Exception:
            if not self._preview_warning_printed:
                print("[camera] Preview unavailable in this environment, continuing without preview")
                self._preview_warning_printed = True
            self.preview_enabled = False

    # ── helpers ──────────────────────────────────────────────────────────────

    def _publish(self, topic: str, msg_type: str, msg: dict) -> str:
        return json.dumps({"op": "publish", "topic": topic, "type": msg_type, "msg": msg})

    def _subscribed_clients(self, topic: str):
        return [ws for ws in self.clients if topic in self.client_subscriptions.get(ws, set())]

    def _header(self, seq=0, frame_id="map"):
        return {
            "seq": seq,
            "stamp": {"secs": int(self.t), "nsecs": int((self.t % 1) * 1e9)},
            "frame_id": frame_id
        }

    # ── message generators ───────────────────────────────────────────────────

    def make_pointcloud2(self):
        """Random 500-point cloud as sensor_msgs/PointCloud2 (rgb8 packed float32 xyz)"""
        n = 200
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
        """
        Webcam frame as sensor_msgs/CompressedImage (JPEG), fallback to
        synthetic gradient. This mirrors what a real robot normally exposes
        via image_transport's compressed plugin on <topic>/compressed — it's
        the realistic way to stream camera frames over a network link, since
        raw rgb8 is ~15-30x larger for the same picture.
        """
        img_array = None  # BGR, for cv2.imencode below

        if self.camera_ready and self.camera is not None:
            ok, frame_bgr = self.camera.read()
            if ok and frame_bgr is not None:
                self._update_preview(frame_bgr)
                # Downscale publish resolution to reduce websocket payload and latency.
                img_array = cv2.resize(frame_bgr, (320, 240), interpolation=cv2.INTER_AREA)
            elif not self._camera_warning_printed:
                print("[camera] Webcam read failed, using synthetic image fallback")
                self._camera_warning_printed = True

        if img_array is None:
            w, h = 320, 240
            rgb = np.zeros((h, w, 3), dtype=np.uint8)
            t = self.t
            for row in range(h):
                r = int(128 + 127 * math.sin(t + row / 30.0))
                g = int(128 + 127 * math.cos(t + row / 40.0))
                b = 100
                rgb[row, :] = [r, g, b]
            img_array = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR) if cv2 is not None else rgb

        if cv2 is not None:
            ok, jpeg_buf = cv2.imencode(
                ".jpg", img_array, [cv2.IMWRITE_JPEG_QUALITY, IMAGE_JPEG_QUALITY]
            )
            if not ok:
                return None
            b64 = base64.b64encode(jpeg_buf.tobytes()).decode()
        else:
            # No OpenCV available to JPEG-encode — this path is only hit if
            # the synthetic fallback is used *and* cv2 is missing. It sends
            # raw bytes so the mock still runs, but latency/fps will suffer;
            # `pip install opencv-python` to get the real compressed path.
            b64 = base64.b64encode(img_array.tobytes()).decode()

        msg = {
            "header": self._header(frame_id="camera"),
            "format": "jpeg",
            "data": b64,
        }
        return self._publish("/image_raw/compressed", "sensor_msgs/CompressedImage", msg)

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
        """Simple telemetry packet with battery, signal, flight mode, a
        3S-LiPo-ish voltage/current pair that drains alongside battery%,
        and an armed flag that flips on/off every ~15s for testing."""
        battery = max(20, 100 - int(self.t * 0.5))
        signal = int(70 + 25 * math.sin(self.t * 0.2))
        modes = ["MANUAL", "AUTO", "GPS", "LOITER"]
        mode = modes[int(self.t / 20) % len(modes)]
        voltage = 12.6 - (100 - np.clip(battery, 0, 100)) * 0.012
        current = 4.5 + 1.5 * math.sin(self.t * 0.3)
        armed = (int(self.t / 15) % 2) == 1

        msg = {
            "battery": int(np.clip(battery, 0, 100)),
            "signal": int(np.clip(signal, 0, 100)),
            "mode": mode,
            "voltage": round(float(voltage), 2),
            "current": round(float(max(0.0, current)), 2),
            "armed": bool(armed),
        }
        return self._publish("/telemetry", "std_msgs/String", msg)

    # ── server ───────────────────────────────────────────────────────────────

    async def handle_client(self, websocket):
        self.clients.add(websocket)
        self.client_subscriptions[websocket] = set()
        self.client_image_busy[websocket] = False
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
                        print(f"    unsubscribe → {topic}")
                    elif op == "advertise":
                        print(f"    advertise → {topic}")
                except Exception:
                    pass
        except Exception:
            pass
        finally:
            self.clients.discard(websocket)
            self.client_subscriptions.pop(websocket, None)
            self.client_image_busy.pop(websocket, None)
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

    async def _send_image_and_clear(self, ws, message: str):
        try:
            await ws.send(message)
        except Exception:
            self.clients.discard(ws)
            self.client_subscriptions.pop(ws, None)
        finally:
            self.client_image_busy[ws] = False

    async def publish_loop(self):
        """Odometry / path / pointcloud / telemetry — unrelated to the image
        loop so a slow image send can never delay these (or vice versa)."""
        frame = 0
        fps = 10
        print("Mock publishing (odom/path/cloud/telemetry) started at ~10 Hz")
        while True:
            self.t += 1.0 / fps

            await self.broadcast(self.make_odometry(), "/Odometry")

            if frame % 3 == 0:
                await self.broadcast(self.make_path(), "/path")

            if frame % 5 == 0:
                await self.broadcast(self.make_pointcloud2(), "/cloud_registered")

            if frame % 2 == 0:
                await self.broadcast(self.make_telemetry(), "/telemetry")

            frame += 1
            await asyncio.sleep(1.0 / fps)

    async def image_loop(self):
        """Camera frames on their own clock at IMAGE_FPS. Runs independently
        of publish_loop, and uses broadcast_image_dropping_stale so a client
        that can't keep up gets fewer frames, never a growing backlog."""
        # Guard against the #1 way this gets WORSE instead of better: if
        # OpenCV isn't installed, make_image() can't JPEG-compress and falls
        # back to sending raw bytes (see below) — pushing raw frames at a
        # high target fps is much worse than the old 5fps raw path. Cap the
        # rate hard in that case and say so loudly.
        effective_fps = IMAGE_FPS if cv2 is not None else min(IMAGE_FPS, 5)
        if cv2 is None:
            print("[image] !! opencv-python not installed -> sending RAW "
                  "uncompressed frames, capped to 5 fps. `pip install "
                  "opencv-python` to enable JPEG compression and reach "
                  "higher fps without flooding the link.")
        else:
            print(f"[image] JPEG compression active (quality={IMAGE_JPEG_QUALITY})")
        print(f"Mock image publishing started at target {effective_fps} fps")

        period = 1.0 / effective_fps
        next_tick = asyncio.get_event_loop().time()

        # Rolling stats printed every ~2s so you can see directly whether the
        # bottleneck is encode speed, achieved fps, or frames being dropped
        # for being stale (link too slow to keep up), instead of guessing.
        stats_window_start = asyncio.get_event_loop().time()
        frames_sent = 0
        frames_dropped_stale = 0
        bytes_sent = 0

        while True:
            loop_start = asyncio.get_event_loop().time()
            msg = self.make_image()
            if msg is not None:
                targets = self._subscribed_clients("/image_raw/compressed")
                for ws in targets:
                    if self.client_image_busy.get(ws):
                        frames_dropped_stale += 1
                        continue
                    self.client_image_busy[ws] = True
                    asyncio.create_task(self._send_image_and_clear(ws, msg))
                if targets:
                    frames_sent += 1
                    bytes_sent += len(msg)

            now = asyncio.get_event_loop().time()
            if now - stats_window_start >= 2.0:
                elapsed = now - stats_window_start
                avg_kb = (bytes_sent / frames_sent / 1024) if frames_sent else 0
                print(f"[image] {frames_sent/elapsed:.1f} fps sent, "
                      f"{frames_dropped_stale} dropped (client too slow), "
                      f"avg frame {avg_kb:.1f} KB")
                stats_window_start = now
                frames_sent = 0
                frames_dropped_stale = 0
                bytes_sent = 0

            next_tick += period
            delay = next_tick - loop_start
            if delay > 0:
                await asyncio.sleep(delay)
            else:
                # We're behind schedule (camera read/encode took too long,
                # or make_image() itself is the bottleneck) — resync instead
                # of trying to burst-catch-up.
                next_tick = asyncio.get_event_loop().time()

    async def start(self):
        print("=" * 50)
        print("rosbridge MOCK server")
        print(f"ws://{HOST}:{PORT}")
        print("Topics: /cloud_registered /image_raw/compressed /path /Odometry /telemetry")
        print("=" * 50)
        async with serve(
            self.handle_client,
            HOST,
            PORT,
            ping_interval=30,
            ping_timeout=30,
            close_timeout=10,
            max_queue=8,
            write_limit=2**20,
        ):
            await asyncio.gather(self.publish_loop(), self.image_loop())


if __name__ == "__main__":
    mock = RosbridgeMock()
    try:
        asyncio.run(mock.start())
    except KeyboardInterrupt:
        print("Stopped.")
    finally:
        if mock.camera is not None:
            mock.camera.release()
        if cv2 is not None:
            cv2.destroyAllWindows()
