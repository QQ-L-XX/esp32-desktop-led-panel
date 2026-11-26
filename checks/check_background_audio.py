"""Verify webpage audio without a desktop window, including foreground handoff."""

import json
import os
from pathlib import Path
import runpy
import subprocess
import time
from urllib.parse import urlencode
from urllib.request import ProxyHandler, Request, build_opener
import wave
import winsound

workspace = Path(__file__).resolve().parents[1]
(workspace / "work").mkdir(exist_ok=True)
target = json.loads((workspace / "work/web_audio_target.json").read_text(encoding="utf-8"))
opener = build_opener(ProxyHandler({}))
status_path = Path(os.environ["LOCALAPPDATA"]) / "DesktopLedPanel/status.json"


def api(path, values=None):
    data = urlencode(values).encode() if values is not None else None
    with opener.open(Request("http://" + target["stationIp"] + "/api/" + path, data=data), timeout=3) as reply:
        return json.load(reply)


def wait_for(predicate, timeout=12):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        try:
            latest = api("state")
        except OSError:
            time.sleep(0.2)
            continue
        if predicate(latest):
            return latest
        time.sleep(0.1)
    raise AssertionError(latest)


initial = wait_for(lambda state: state["desktopConnected"], timeout=25)
initial_mode = initial["mode"]
initial_brightness = initial["brightness"]
background_pid = json.loads(status_path.read_text(encoding="utf-8"))["pid"]
app = runpy.run_path(str(workspace / "desktop/音乐律动.pyw"))
np = app["np"]
t = np.arange(int(48000 * 0.7)) / 48000
envelope = np.minimum(1, np.minimum(t, t[-1] - t) * 80)
signal = 0.06 * np.sin(2 * np.pi * 880 * t) * envelope
tone_path = workspace / "work/background-audio-check.wav"
with wave.open(str(tone_path), "wb") as tone:
    tone.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
    tone.writeframes((signal * 32767).astype("<i2").tobytes())

try:
    for mode in ("waveform", "spectrum"):
        api("mode", {"mode": mode})
        wait_for(lambda state: state["mode"] == mode and state["audioStreaming"])
        winsound.PlaySound(str(tone_path), winsound.SND_FILENAME | winsound.SND_ASYNC)
        observed = wait_for(lambda state: state["mode"] == mode and state["audioActive"], timeout=4)
        assert observed["owner"] == "phone"
        print("PASS background webpage audio with actual test sound:", mode, flush=True)
        time.sleep(0.8)
    api("mode", {"mode": "pet"})
    idle = wait_for(lambda state: state["mode"] == "pet" and state["desktopAudioStatus"] == "idle")
    assert not idle["audioStreaming"]
    print("PASS non-audio modes release the playback capture", flush=True)

    api("mode", {"mode": initial_mode})
    tk = app["tk"]
    real_tk = tk.Tk
    handoff = []

    def test_window():
        root = real_tk()
        root.withdraw()
        deadline = time.monotonic() + 8

        def check():
            try:
                state = api("state")
                background = json.loads(status_path.read_text(encoding="utf-8"))
                if background["phase"] == "desktop" and state["desktopConnected"]:
                    handoff.append(True)
                    root.destroy()
                elif time.monotonic() >= deadline:
                    root.destroy()
                else:
                    root.after(100, check)
            except Exception:
                root.destroy()
                raise

        root.after(300, check)
        return root

    tk.Tk = test_window
    app["run"]()
    assert handoff, "Background did not yield to the desktop program"
    wait_for(lambda state: state["desktopConnected"])
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        background = json.loads(status_path.read_text(encoding="utf-8"))
        if background["phase"] in ("connected", "audio"):
            break
        time.sleep(0.1)
    else:
        raise AssertionError(background)
    assert background["pid"] == background_pid
    print("PASS desktop window takes over, then background resumes automatically", flush=True)

    duplicate = subprocess.Popen([
        "C:/Program Files/Python311/pythonw.exe",
        str(workspace / "desktop/音乐律动.pyw"), "--background",
    ], creationflags=subprocess.CREATE_NO_WINDOW)
    assert duplicate.wait(timeout=15) == 0
    assert json.loads(status_path.read_text(encoding="utf-8"))["pid"] == background_pid
    print("PASS duplicate background launches leave one owner", flush=True)
finally:
    winsound.PlaySound(None, 0)
    api("mode", {"mode": initial_mode})
    api("brightness", {"value": initial_brightness})

print("Background-only music control checks passed", flush=True)
