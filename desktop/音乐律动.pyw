from collections import Counter, deque
from contextlib import contextmanager
from ctypes import Structure, byref, c_uint64, c_void_p, sizeof, windll
from ctypes import wintypes
from datetime import datetime, timedelta, timezone
import json
import math
import os
from pathlib import Path
from queue import Empty, SimpleQueue
import shutil
import sys
from threading import Thread
import time
import tkinter as tk
from tkinter import messagebox
from urllib.request import Request, urlopen

import numpy as np
import pyaudiowpatch as pyaudio

import serial
from serial.tools import list_ports

kernel32 = windll.kernel32
kernel32.CreateMutexW.argtypes = (c_void_p, wintypes.BOOL, wintypes.LPCWSTR)
kernel32.CreateMutexW.restype = wintypes.HANDLE
kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.ReleaseMutex.argtypes = (wintypes.HANDLE,)
kernel32.ReleaseMutex.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.CreateEventW.argtypes = (c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR)
kernel32.CreateEventW.restype = wintypes.HANDLE
kernel32.OpenEventW.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR)
kernel32.OpenEventW.restype = wintypes.HANDLE
kernel32.SetEvent.argtypes = (wintypes.HANDLE,)
kernel32.SetEvent.restype = wintypes.BOOL
kernel32.ResetEvent.argtypes = (wintypes.HANDLE,)
kernel32.ResetEvent.restype = wintypes.BOOL

# Configure your own board explicitly; never select an arbitrary USB controller.
def configured_panel_mac():
    value = os.environ.get("LED_PANEL_MAC", "")
    config_path = Path(__file__).with_name("panel.local.json")
    if not value and config_path.is_file():
        value = json.loads(config_path.read_text(encoding="utf-8")).get("mac", "")
    return str(value).replace(":", "").replace("-", "").upper()


PANEL_MAC = configured_panel_mac()
PANEL_LOCK_ID = PANEL_MAC or "Unconfigured"
FOREGROUND_MUTEX = rf"Local\DesktopLedPanel-{PANEL_LOCK_ID}-Foreground"
BACKGROUND_MUTEX = rf"Local\DesktopLedPanel-{PANEL_LOCK_ID}-Background"
BACKGROUND_STOP_EVENT = rf"Local\DesktopLedPanel-{PANEL_LOCK_ID}-Stop"


@contextmanager
def panel_process_lock(name):
    handle = kernel32.CreateMutexW(None, False, name)
    if not handle:
        raise OSError("无法创建灯板进程锁")
    acquired = kernel32.WaitForSingleObject(handle, 0) in (0, 128)
    try:
        yield acquired
    finally:
        if acquired:
            kernel32.ReleaseMutex(handle)
        kernel32.CloseHandle(handle)


def foreground_is_running(handle):
    result = kernel32.WaitForSingleObject(handle, 0)
    if result in (0, 128):
        kernel32.ReleaseMutex(handle)
        return False
    if result != 258:
        raise OSError("无法查询电脑控制窗口")
    return True


class Spectrum:
    def __init__(self, rate):
        self.rate = rate
        self.size = 4096
        self.window = np.hanning(self.size)
        self.samples = np.zeros((self.size, 2), dtype=np.float32)
        self.edges = np.rint(np.geomspace(43, min(16000, rate * 0.45), 17) * self.size / rate).astype(int)
        for index in range(1, len(self.edges)):
            self.edges[index] = max(self.edges[index], self.edges[index - 1] + 1)
        self.levels = np.zeros(16)

    def update(self, samples):
        count = len(samples)
        self.samples[:-count] = self.samples[count:]
        self.samples[-count:] = samples
        bins = np.abs(np.fft.rfft(self.samples * self.window[:, None], axis=0))
        bins = bins.max(axis=1) * (2 / self.window.sum())
        bands = np.array([bins[start:end].max() for start, end in zip(self.edges[:-1], self.edges[1:])])
        decibels = 20 * np.log10(np.maximum(bands, 1e-8))
        target = np.clip((decibels + 56) * (8 / 45), 0, 8)
        self.levels = np.maximum(target, self.levels - 0.85)
        return np.rint(self.levels).astype(np.uint8)


def panel_port():
    if len(PANEL_MAC) != 12 or any(char not in "0123456789ABCDEF" for char in PANEL_MAC):
        raise RuntimeError("请先在 desktop/panel.local.json 或 LED_PANEL_MAC 中填写你的 ESP32-C3 MAC。")
    for port in list_ports.comports():
        serial_mac = (port.serial_number or "").replace(":", "").replace("-", "").upper()
        if (port.vid, port.pid) == (0x303A, 0x1001) and serial_mac == PANEL_MAC:
            return port.device
    raise RuntimeError("找不到已配置的 ESP32-C3。请检查 MAC 和 USB 数据线。")


def connect_panel(wait=0):
    connection = serial.Serial()
    connection.port = panel_port()
    connection.baudrate = 115200
    connection.timeout = 0.08
    connection.write_timeout = 0.3
    connection.dtr = connection.rts = False
    deadline = time.monotonic() + wait
    while True:
        try:
            connection.open()
            return connection
        except serial.SerialException:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)


def read_panel_messages(connection, pending):
    pending.extend(connection.read_all())
    messages = []
    while b"\n" in pending:
        line, _, remaining = pending.partition(b"\n")
        pending[:] = remaining
        messages.append(line.decode(errors="replace").strip())
    return messages


def parse_panel_state(message):
    if not message.startswith("Panel state: "):
        return None
    try:
        state = json.loads(message.removeprefix("Panel state: "))
    except ValueError:
        return None
    return state if isinstance(state, dict) else None


def read_panel_state(connection):
    pending = bytearray()
    deadline = time.monotonic() + 1.5
    next_query = 0.0
    while time.monotonic() < deadline:
        if time.monotonic() >= next_query:
            connection.write(b"?")
            next_query = time.monotonic() + 0.35
        for message in read_panel_messages(connection, pending):
            state = parse_panel_state(message)
            if state:
                return state
        time.sleep(0.01)
    return {}


def send_levels(port, levels):
    port.write(b"F" + bytes(48 + int(level) for level in levels) + b"\n")


def send_waveform(port, rows):
    port.write(b"W" + bytes(97 + int(row) for row in rows) + b"\n")


class MemoryStatus(Structure):
    _fields_ = [
        ("length", wintypes.DWORD), ("load", wintypes.DWORD),
        *((name, c_uint64) for name in (
            "total_physical", "available_physical", "total_page", "available_page",
            "total_virtual", "available_virtual", "available_extended",
        )),
    ]


def cpu_times():
    times = [wintypes.FILETIME() for _ in range(3)]
    if not windll.kernel32.GetSystemTimes(*(byref(value) for value in times)):
        raise OSError("无法读取 CPU 占用率")
    return tuple(value.dwLowDateTime | value.dwHighDateTime << 32 for value in times)


def computer_status(previous_cpu):
    current = cpu_times()
    idle, kernel, user = (new - old for new, old in zip(current, previous_cpu))
    total = kernel + user
    cpu = round(100 * (total - idle) / total) if total else 0
    memory = MemoryStatus()
    memory.length = sizeof(memory)
    if not windll.kernel32.GlobalMemoryStatusEx(byref(memory)):
        raise OSError("无法读取内存占用率")
    disk = shutil.disk_usage(os.environ.get("SystemDrive", "C:") + "\\")
    drive = round(100 * disk.used / disk.total)
    return (max(0, min(100, cpu)), memory.load, drive), current


FUZHOU_WEATHER_URL = "https://wttr.in/Fuzhou,Fujian?format=j1"
FUZHOU_TIMEZONE = timezone(timedelta(hours=8))


def read_fuzhou_weather():
    request = Request(FUZHOU_WEATHER_URL, headers={"User-Agent": "ESP32DesktopPanel/1.0"})
    with urlopen(request, timeout=8) as response:
        data = json.load(response)
    area = data["nearest_area"][0]
    if (area["areaName"][0]["value"].casefold(), area["region"][0]["value"].casefold()) != ("fuzhou", "fujian"):
        raise ValueError("天气服务返回的地点不是福建福州")
    report = data["current_condition"][0]
    temperature = float(report["temp_C"])
    if not math.isfinite(temperature) or not -99 <= temperature <= 99:
        raise ValueError("气温数据超出范围")
    description = report["weatherDesc"][0]["value"].casefold()

    if "thunder" in description:
        icon, condition = 4, "雷雨"
    elif "snow" in description or "sleet" in description or "ice pellets" in description:
        icon, condition = 5, "雨雪"
    elif "rain" in description or "drizzle" in description:
        icon, condition = 3, "有雨"
    elif "haze" in description or "smok" in description or "smog" in description:
        icon, condition = 6, "霾"
    elif "dust" in description or "sand" in description:
        icon, condition = 6, "沙尘"
    elif "mist" in description or "fog" in description:
        icon, condition = 6, "雾"
    elif "cloud" in description or "overcast" in description:
        icon, condition = 2, "多云" if "partly" in description else "阴"
    elif "sunny" in description or "clear" in description:
        local_now = datetime.now(FUZHOU_TIMEZONE)
        today = data["weather"][0]
        if today["date"] == local_now.date().isoformat():
            astronomy = today["astronomy"][0]
            sunrise = datetime.strptime(astronomy["sunrise"], "%I:%M %p").time()
            sunset = datetime.strptime(astronomy["sunset"], "%I:%M %p").time()
            daylight = sunrise <= local_now.time() < sunset
        else:
            daylight = 6 <= local_now.hour < 18
        icon, condition = (0 if daylight else 1), "晴"
    else:
        icon, condition = 7, "天气类型未知"
    return round(temperature), icon, condition


class FuzhouWeather:
    def __init__(self):
        self.results = SimpleQueue()
        self.loading = False
        self.next_fetch = 0.0
        self.latest = None
        self.last_success = 0.0

    def fetch(self):
        try:
            result = read_fuzhou_weather()
        except (OSError, ValueError, KeyError, IndexError, TypeError):
            result = None
        self.results.put(result)

    def poll(self, now):
        try:
            result = self.results.get_nowait()
        except Empty:
            pass
        else:
            self.loading = False
            self.next_fetch = now + (120 if result is None else 600)
            if result is not None:
                self.latest = result
                self.last_success = now

        if not self.loading and now >= self.next_fetch:
            self.loading = True
            Thread(target=self.fetch, daemon=True).start()
        if self.latest is not None and now - self.last_success < 3600:
            return self.latest
        return None


def calendar_text(current, weather):
    message = f"{current:%H:%M:%S%m-%d}{current.weekday()}"
    if weather is not None:
        temperature, icon, _ = weather
        message += f"{temperature:+03d}{icon}"
    return message


def decode_block(data, channels):
    samples = np.frombuffer(data, dtype=np.float32).reshape(-1, channels)
    if channels == 1:
        samples = np.repeat(samples, 2, axis=1)
    return samples


class Waveform:
    columns = 32
    height = 8

    def __init__(self, rate):
        self.column_samples = max(1, round(rate * 0.012 / self.columns))
        self.span = self.columns * self.column_samples
        self.kernel = np.hanning(round(rate * 0.0014) | 1)
        self.kernel /= self.kernel.sum()
        self.level = 0.06
        self.blank = np.full(self.columns, self.height, dtype=np.uint8)
        self.previous = None

    def update(self, samples):
        if len(samples) < self.span:
            return self.blank
        energy = np.mean(samples * samples, axis=0)
        if energy.max() < 0.002**2:
            self.previous = None
            return self.blank

        signal = samples.mean(axis=1)
        if np.mean(signal * signal) < 0.1 * energy.max():
            signal = samples[:, int(energy.argmax())]
        signal = signal - signal.mean()
        signal = np.convolve(signal, self.kernel, mode="same")
        # Start on a rising zero crossing when there is room for a full trace.
        search_end = len(signal) - self.span + 1
        threshold = max(0.001, np.abs(signal).max() * 0.08)
        crossings = np.flatnonzero((signal[:search_end - 1] <= threshold) &
                                   (signal[1:search_end] > threshold))
        if crossings.size:
            start = int(crossings[-1]) + 1
        else:
            start = search_end - 1
        trace = signal[start:start + self.span].reshape(self.columns, self.column_samples).mean(axis=1)
        peak = np.abs(trace).max()
        self.level = max(0.06, peak, self.level * 0.94)
        middle = (self.height - 1) / 2
        rows = middle - trace * (middle - 0.4) / self.level
        if self.previous is not None:
            rows = 0.72 * rows + 0.28 * self.previous
        self.previous = rows
        return np.rint(rows).clip(0, self.height - 1).astype(np.uint8)


def open_audio(audio, pending):
    device = audio.get_default_wasapi_loopback()
    rate = int(device["defaultSampleRate"])
    channels = min(2, int(device["maxInputChannels"]))
    if not 32000 <= rate <= 192000 or channels < 1:
        raise RuntimeError("当前播放设备的音频格式无法使用。")

    def on_audio(data, frame_count, time_info, status):
        pending.append(data)
        return None, pyaudio.paContinue

    stream = audio.open(
        format=pyaudio.paFloat32, channels=channels, rate=rate, input=True,
        input_device_index=device["index"], frames_per_buffer=min(round(rate / 30), 2048),
        stream_callback=on_audio,
    )
    return stream, Spectrum(rate), Waveform(rate), channels


def run_desktop(test=False):
    with pyaudio.PyAudio() as audio:
        pending = deque(maxlen=1)
        connection = connect_panel(wait=3)
        with connection:
            if test:
                capture, analyzer, waveform, channels = open_audio(audio, pending)
                with capture:
                    run_audio_test(connection, analyzer, waveform, pending, channels)
                return
            initial_state = read_panel_state(connection)
            supports_offline = isinstance(initial_state, dict) and bool(initial_state)
            supports_web_audio = supports_offline and initial_state.get("webAudioControl") is True
            root = tk.Tk()
            root.title("桌面灯板")
            root.resizable(False, False)
            root.configure(padx=20, pady=15)
            status = tk.StringVar(value="正在同步电脑时间")
            tk.Label(root, textvariable=status, width=31).pack(pady=8)
            initial_mode = initial_state.get("mode", "calendar")
            if initial_mode not in ("calendar", "status", "waveform", "spectrum", "pet", "dino", "lamp", "newyear", "phrases"):
                initial_mode = "calendar"
            if "--pet" in sys.argv:
                initial_mode = "pet"
            elif "--dino" in sys.argv:
                initial_mode = "dino"
            mode = tk.StringVar(value=initial_mode)
            control_requested = True
            last_brightness = None

            def request_desktop_control():
                nonlocal control_requested
                control_requested = True

            def brightness_changed(value):
                if int(float(value)) != last_brightness:
                    request_desktop_control()

            modes = tk.Frame(root)
            modes.pack(pady=(0, 8))
            options = (
                ("日历时钟", "calendar"), ("电脑状态", "status"),
                ("彩色波形", "waveform"), ("频谱柱", "spectrum"),
                ("桌面宠物", "pet"), ("小恐龙", "dino"),
                ("照明", "lamp"), ("新年动画", "newyear"),
                ("中文语句", "phrases"),
            )
            for index, (label, value) in enumerate(options):
                tk.Radiobutton(modes, text=label, variable=mode, value=value,
                               indicatoron=False, width=12, padx=6, pady=5,
                               command=request_desktop_control).grid(
                                   row=index // 2, column=index % 2, padx=3, pady=3)
            game_controls = tk.Frame(root)
            score_text = tk.StringVar(value="分数 0")
            tk.Label(game_controls, textvariable=score_text, width=9).pack(side=tk.LEFT)
            jump_label = tk.StringVar(value="开始 · 空格")
            game_ready = False

            def jump(event=None):
                if mode.get() == "dino":
                    try:
                        if game_ready:
                            connection.write(b"J")
                    except (OSError, serial.SerialException) as error:
                        status.set(f"连接中断：{error}")
                    return "break"

            tk.Button(game_controls, textvariable=jump_label, width=15,
                      command=jump, takefocus=False).pack(side=tk.LEFT)
            root.bind("<KeyPress-space>", jump)
            def interact_with_pet(command):
                try:
                    connection.write(command)
                except (OSError, serial.SerialException) as error:
                    status.set(f"连接中断：{error}")

            pet_controls = tk.Frame(root)
            tk.Button(pet_controls, text="摸摸", width=9,
                      command=lambda: interact_with_pet(b"Y")).pack(side=tk.LEFT, padx=3)
            tk.Button(pet_controls, text="玩水", width=9,
                      command=lambda: interact_with_pet(b"Z")).pack(side=tk.LEFT, padx=3)
            initial_brightness = initial_state.get("brightness", 38)
            if not isinstance(initial_brightness, (int, float)) or not 0 <= initial_brightness <= 100:
                initial_brightness = 38
            brightness = tk.IntVar(value=int(initial_brightness))
            tk.Scale(root, label="整体亮度（%）", from_=0, to=100,
                     variable=brightness, command=brightness_changed,
                     orient=tk.HORIZONTAL, length=250).pack(pady=(0, 6))
            stop_button = tk.Button(root, text="关闭窗口", width=12, command=root.destroy)
            stop_button.pack(pady=3)
            last_data = time.monotonic()
            silence = np.zeros(16, dtype=np.uint8)
            blank_wave = np.full(Waveform.columns, Waveform.height, dtype=np.uint8)
            last_mode = None
            capture = None
            previous_cpu = None
            last_desktop_update = 0.0
            phone_control = False
            phone_mode = ""
            last_state_request = 0.0
            last_clock_sync = 0.0
            last_takeover_at = 0.0
            last_heartbeat = 0.0
            audio_state = 0
            last_audio_attempt = 0.0
            weather = FuzhouWeather()
            panel_replies = bytearray()
            game_messages = {
                "ready": ("空格开始", "开始 · 空格"),
                "running": ("空格跳过仙人掌", "跳跃 · 空格"),
                "jump": ("空格跳过仙人掌", "跳跃 · 空格"),
                "passed": ("空格跳过仙人掌", "跳跃 · 空格"),
                "crashed": ("撞到仙人掌了，空格重开", "重开 · 空格"),
            }
            pet_messages = {
                "awake": "小鸭在游泳",
                "sleeping": "小鸭在打盹",
                "petted": "小鸭收到了摸摸",
                "playing": "小鸭在玩水",
            }

            def show_silence(selected):
                if selected == "waveform":
                    send_waveform(connection, blank_wave)
                else:
                    send_levels(connection, silence)

            def tick():
                nonlocal last_data, last_mode, game_ready
                nonlocal capture, analyzer, waveform, channels
                nonlocal previous_cpu, last_desktop_update
                nonlocal last_brightness
                nonlocal control_requested, phone_control, phone_mode
                nonlocal last_state_request, last_clock_sync, last_takeover_at
                nonlocal last_heartbeat, audio_state, last_audio_attempt
                try:
                    data = pending.pop() if pending else None
                    selected = mode.get()
                    now = time.monotonic()
                    replies = read_panel_messages(connection, panel_replies)
                    if supports_offline:
                        if supports_web_audio and now - last_heartbeat >= 1:
                            connection.write(f"O{audio_state}\n".encode("ascii"))
                            last_heartbeat = now
                        for reply in replies:
                            state = parse_panel_state(reply)
                            if not isinstance(state, dict) or control_requested or now - last_takeover_at < 0.5:
                                continue
                            phone_control = state.get("owner") == "phone"
                            if phone_control:
                                phone_mode = state.get("mode", "")
                            saved_level = state.get("brightness", last_brightness)
                            if isinstance(saved_level, (int, float)) and 0 <= saved_level <= 100:
                                last_brightness = int(saved_level)
                                brightness.set(last_brightness)
                        if now - last_state_request >= 1:
                            connection.write(b"?")
                            last_state_request = now
                        if now - last_clock_sync >= 30:
                            connection.write(f"E{int(time.time())}\n".encode("ascii"))
                            last_clock_sync = now
                        if control_requested:
                            if phone_control or selected != last_mode:
                                last_mode = None
                            control_requested = False
                            phone_control = False
                            last_takeover_at = now
                            connection.write(f"M{selected}\n".encode("ascii"))
                            last_brightness = None
                        web_audio = supports_web_audio and phone_control and phone_mode in ("waveform", "spectrum")
                        if web_audio:
                            selected = phone_mode
                            mode.set(selected)
                        elif phone_control:
                            if capture is not None:
                                capture.close()
                                capture = None
                            audio_state = 0
                            last_audio_attempt = 0.0
                            game_controls.pack_forget()
                            pet_controls.pack_forget()
                            names = dict((value, label) for label, value in options)
                            status.set(f"手机正在控制 · {names.get(phone_mode, phone_mode)}\n点任一模式可切回电脑")
                            root.after(24, tick)
                            return
                    else:
                        web_audio = False
                    current_brightness = brightness.get()
                    if current_brightness != last_brightness:
                        connection.write(f"V{current_brightness:03d}\n".encode("ascii"))
                        last_brightness = current_brightness
                    if selected != last_mode:
                        if last_mode == "lamp":
                            connection.write(b"!")
                        last_desktop_update = 0.0
                        game_ready = False
                        panel_replies.clear()
                        if selected in ("waveform", "spectrum"):
                            data = None
                        else:
                            if capture is not None:
                                capture.close()
                                capture = None
                            audio_state = 0
                        if selected == "dino":
                            connection.write(b"G")
                            score_text.set("分数 0")
                            status.set("正在打开小恐龙")
                            game_controls.pack(before=stop_button, pady=(0, 6))
                            root.focus_set()
                        else:
                            game_controls.pack_forget()
                        if selected == "pet":
                            connection.write(b"X")
                            status.set("小鸭在灯板上")
                            pet_controls.pack(before=stop_button, pady=(0, 6))
                        else:
                            pet_controls.pack_forget()
                        if selected == "lamp":
                            connection.write(b"I8\n")
                        if selected == "newyear":
                            connection.write(b"N")
                        if selected == "status":
                            previous_cpu = cpu_times()
                            connection.write(b"!")
                            status.set("正在读取电脑状态")
                            last_desktop_update = now - 0.85
                    if selected in ("waveform", "spectrum") and capture is None and (
                            selected != last_mode or now - last_audio_attempt >= 5):
                        pending.clear()
                        last_audio_attempt = now
                        try:
                            capture, analyzer, waveform, channels = open_audio(audio, pending)
                            audio_state = 1
                        except (OSError, RuntimeError):
                            audio_state = 2
                            status.set("当前播放设备无法取声，正在重试")
                        data = None
                    if selected == "dino":
                        for reply in replies:
                            if reply.startswith("Dino 8x32: "):
                                event, _, score = reply.removeprefix("Dino 8x32: ").partition(", score=")
                                if event in game_messages and score.isdigit():
                                    game_ready = True
                                    score_text.set(f"分数 {score}")
                                    message, button_text = game_messages[event]
                                    status.set(message)
                                    jump_label.set(button_text)
                    elif selected == "pet":
                        for reply in replies:
                            event = reply.removeprefix("Pet 8x32 duck: ")
                            if event in pet_messages:
                                status.set(pet_messages[event])
                    elif selected == "calendar":
                        if now - last_desktop_update >= 0.5:
                            current = datetime.now()
                            snapshot = weather.poll(now)
                            connection.write(b"Q" + calendar_text(current, snapshot).encode("ascii") + b"\n")
                            weekday = "一二三四五六日"[current.weekday()]
                            forecast = "天气暂不可用"
                            if snapshot is not None:
                                temperature, _, condition = snapshot
                                forecast = f"{condition}  {temperature}°C"
                            status.set(current.strftime("%m-%d  %H:%M:%S") +
                                       f"  周{weekday}\n福州 · {forecast}")
                            last_desktop_update = now
                    elif selected == "lamp":
                        status.set(f"白光照明  {current_brightness}%" if current_brightness else "照明已关闭")
                    elif selected == "newyear":
                        status.set("灯板正在播放新年动画")
                    elif selected == "phrases":
                        status.set("灯板正在轮播中文语句\n可在手机网页中编辑和收藏词句")
                    elif selected == "status":
                        if now - last_desktop_update >= 1:
                            values, previous_cpu = computer_status(previous_cpu)
                            connection.write(b"K" + "".join(f"{value:03d}" for value in values).encode("ascii") + b"\n")
                            cpu, memory, drive = values
                            status.set(f"CPU {cpu}%   内存 {memory}%   系统盘 {drive}%")
                            last_desktop_update = now
                    elif data is not None and capture is not None:
                        samples = decode_block(data, channels)
                        if selected == "waveform":
                            rows = waveform.update(samples)
                            send_waveform(connection, rows)
                            active = np.any(rows < Waveform.height)
                        else:
                            levels = analyzer.update(samples[-analyzer.size:])
                            send_levels(connection, levels)
                            active = levels.any()
                        last_data = now
                        message = "跟随电脑声音" if active else "静音跑马灯"
                        status.set(("网页控制 · " if web_audio else "") + message)
                    elif capture is not None and (selected != last_mode or now - last_data > 0.2):
                        show_silence(selected)
                        status.set(("网页控制 · " if web_audio else "") + "静音跑马灯")
                    last_mode = selected
                except (OSError, serial.SerialException) as error:
                    status.set(f"连接中断：{error}")
                    return
                root.after(24, tick)

            root.after(0, tick)
            try:
                root.mainloop()
            finally:
                if capture is not None:
                    capture.close()
            if connection.is_open:
                try:
                    connection.write(b"~" if supports_offline else b"!")
                except (OSError, serial.SerialException):
                    pass


def run(test=False):
    with panel_process_lock(FOREGROUND_MUTEX) as acquired:
        if not acquired:
            raise RuntimeError("桌面灯板控制窗口已经运行。")
        run_desktop(test=test)


def stop_background():
    handle = kernel32.OpenEventW(0x0002, False, BACKGROUND_STOP_EVENT)
    if handle:
        try:
            kernel32.SetEvent(handle)
        finally:
            kernel32.CloseHandle(handle)


def run_background():
    with panel_process_lock(BACKGROUND_MUTEX) as acquired:
        if not acquired:
            return
        foreground_probe = kernel32.CreateMutexW(None, False, FOREGROUND_MUTEX)
        stop_event = kernel32.CreateEventW(None, True, False, BACKGROUND_STOP_EVENT)
        if not foreground_probe or not stop_event:
            if foreground_probe:
                kernel32.CloseHandle(foreground_probe)
            if stop_event:
                kernel32.CloseHandle(stop_event)
            raise OSError("无法启动灯板后台")
        kernel32.ResetEvent(stop_event)
        try:
            background_loop(foreground_probe, stop_event)
        finally:
            kernel32.CloseHandle(foreground_probe)
            kernel32.CloseHandle(stop_event)


def background_loop(foreground_probe, stop_event):
    runtime_dir = Path(os.environ["LOCALAPPDATA"]) / "DesktopLedPanel"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    status_path = runtime_dir / "status.json"
    previous_status = None
    last_status_at = 0.0
    connection = None
    audio = None
    capture = None
    pending = deque(maxlen=1)
    replies = bytearray()
    state = {}
    audio_state = 0
    last_audio_attempt = 0.0
    last_heartbeat = 0.0
    last_query = 0.0
    last_reply = 0.0
    last_audio_data = 0.0
    blank_wave = np.full(Waveform.columns, Waveform.height, dtype=np.uint8)
    silence = np.zeros(16, dtype=np.uint8)

    def publish(phase, error=""):
        nonlocal previous_status, last_status_at
        mode = state.get("mode", "")
        signature = (phase, mode, audio_state, error)
        now = time.monotonic()
        if signature == previous_status and now - last_status_at < 30:
            return
        snapshot = {
            "pid": os.getpid(), "phase": phase, "mode": mode,
            "audioState": audio_state, "error": error, "updatedAt": time.time(),
        }
        temporary = status_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(snapshot, ensure_ascii=False), encoding="utf-8")
        os.replace(temporary, status_path)
        previous_status = signature
        last_status_at = now

    def close_audio():
        nonlocal capture, audio, audio_state, last_audio_attempt
        if capture is not None:
            try:
                capture.close()
            except OSError:
                pass
            capture = None
        if audio is not None:
            try:
                audio.terminate()
            except OSError:
                pass
            audio = None
        pending.clear()
        audio_state = 0
        last_audio_attempt = 0.0

    def close_device():
        nonlocal connection, state
        close_audio()
        if connection is not None:
            try:
                if connection.is_open:
                    connection.write(b"~")
            except (OSError, serial.SerialException):
                pass
            try:
                connection.close()
            except OSError:
                pass
            connection = None
        state = {}
        replies.clear()

    try:
        while kernel32.WaitForSingleObject(stop_event, 0) != 0:
            if foreground_is_running(foreground_probe):
                if connection is not None:
                    close_device()
                publish("desktop")
                kernel32.WaitForSingleObject(stop_event, 100)
                continue
            try:
                if connection is None:
                    connection = connect_panel()
                    state = read_panel_state(connection)
                    if not state.get("webAudioControl"):
                        raise RuntimeError("灯板尚未就绪，等待重新连接")
                    last_query = last_heartbeat = last_audio_attempt = 0.0
                    last_reply = time.monotonic()
                    last_audio_data = 0.0
                for message in read_panel_messages(connection, replies):
                    updated = parse_panel_state(message)
                    if updated:
                        state = updated
                        last_reply = time.monotonic()
                now = time.monotonic()
                if now - last_reply > 6:
                    raise RuntimeError("灯板状态暂时无响应，正在重新连接")
                selected = state.get("mode")
                if selected in ("waveform", "spectrum"):
                    if capture is None and now - last_audio_attempt >= 5:
                        last_audio_attempt = now
                        try:
                            if audio is None:
                                audio = pyaudio.PyAudio()
                            capture, analyzer, waveform, channels = open_audio(audio, pending)
                            audio_state = 1
                        except (OSError, RuntimeError):
                            audio_state = 2
                    if capture is not None:
                        data = pending.pop() if pending else None
                        if data is not None:
                            samples = decode_block(data, channels)
                            if selected == "waveform":
                                send_waveform(connection, waveform.update(samples))
                            else:
                                send_levels(connection, analyzer.update(samples[-analyzer.size:]))
                            last_audio_data = now
                        elif now - last_audio_data >= 0.15:
                            if selected == "waveform":
                                send_waveform(connection, blank_wave)
                            else:
                                send_levels(connection, silence)
                            last_audio_data = now
                elif capture is not None or audio is not None:
                    close_audio()
                if now - last_heartbeat >= 1:
                    connection.write(f"O{audio_state}\n".encode("ascii"))
                    last_heartbeat = now
                if now - last_query >= 0.5:
                    connection.write(b"?")
                    last_query = now
                publish("audio" if capture is not None else "connected")
                kernel32.WaitForSingleObject(stop_event, 15 if capture is not None else 80)
            except (OSError, RuntimeError, serial.SerialException) as error:
                close_device()
                publish("waiting", str(error))
                kernel32.WaitForSingleObject(stop_event, 1000)
    finally:
        close_device()
        publish("stopped")


def run_audio_test(connection, analyzer, waveform, pending, channels):
    import winsound
    import wave

    # A short, quiet local tone verifies the speaker-to-panel path.
    work_dir = Path(__file__).resolve().parents[1] / "work"
    work_dir.mkdir(exist_ok=True)
    tone_path = work_dir / "music-loopback-check.wav"
    t = np.arange(48000) / 48000
    tones = np.concatenate([
        0.15 * np.sin(2 * np.pi * frequency * t) * np.minimum(1, np.minimum(t, 1 - t) * 60)
        for frequency in (220, 1760)
    ])
    with wave.open(str(tone_path), "wb") as tone:
        tone.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
        tone.writeframes((tones * 32767).astype("<i2").tobytes())
    winsound.PlaySound(str(tone_path), winsound.SND_FILENAME | winsound.SND_ASYNC)
    counts = Counter()
    waveform_spans = []
    replies = bytearray()
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if pending:
            samples = decode_block(pending.pop(), channels)
            levels = analyzer.update(samples[-analyzer.size:])
            rows = waveform.update(samples)
            if levels.max() >= 2:
                counts[int(levels.argmax())] += 1
            send_levels(connection, levels)
            send_waveform(connection, rows)
            if np.all(rows < Waveform.height):
                waveform_spans.append(int(np.ptp(rows)))
        replies.extend(connection.read_all())
        time.sleep(0.005)
    print("Loopback peak bands:", sorted(counts.items()))
    print("Waveform spans:", Counter(waveform_spans))
    print(replies.decode(errors="replace").strip())
    assert any(3 <= band <= 5 and count >= 8 for band, count in counts.items())
    assert any(9 <= band <= 11 and count >= 8 for band, count in counts.items())
    assert b"Music 8x32:" in replies
    assert sum(span >= 4 for span in waveform_spans) >= 8
    assert b"Waveform 8x32:" in replies


if __name__ == "__main__":
    if "--stop-background" in sys.argv:
        stop_background()
    elif "--background" in sys.argv:
        run_background()
    elif "--test" in sys.argv:
        run(test=True)
    else:
        try:
            run()
        except Exception as error:
            messagebox.showerror("灯板音乐律动", str(error))
