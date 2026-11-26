"""Exercise web-selected audio through the real desktop loop and lamp firmware."""

import json
from pathlib import Path
import runpy
import time
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import ProxyHandler, Request, build_opener
import wave
import winsound


workspace = Path(__file__).resolve().parents[1]
(workspace / "work").mkdir(exist_ok=True)
target = json.loads((workspace / "work/web_audio_target.json").read_text(encoding="utf-8"))
base_url = "http://" + target["stationIp"]
opener = build_opener(ProxyHandler({}))


def api(path, values=None, expected=200):
    data = urlencode(values).encode() if values is not None else None
    request = Request(base_url + "/api/" + path, data=data)
    try:
        with opener.open(request, timeout=3) as response:
            code, body = response.status, response.read()
    except HTTPError as error:
        code, body = error.code, error.read()
    assert code == expected, (path, code, body[:180])
    return json.loads(body)


initial = api("state")
assert initial["webAudioControl"] and not initial["desktopConnected"], initial
for mode in ("waveform", "spectrum"):
    api("mode", {"mode": mode}, expected=409)
assert api("state")["mode"] == target["mode"]
print("Offline web audio requests give a clear connection error and preserve the mode", flush=True)

app = runpy.run_path(str(workspace / "desktop/音乐律动.pyw"))
tk, serial = app["tk"], app["serial"]
real_tk, real_write = tk.Tk, serial.Serial.write
activity = {"waveform": False, "spectrum": False}
frame_counts = {"waveform": 0, "spectrum": 0}
errors = []
passed = []

np = app["np"]
rate = 48000
samples = np.arange(int(rate * 0.7)) / rate
fade = np.minimum(1, np.minimum(samples, samples[-1] - samples) * 80)
sound = 0.06 * np.sin(2 * np.pi * 660 * samples) * fade
tone_path = workspace / "work/web-audio-check.wav"
with wave.open(str(tone_path), "wb") as tone:
    tone.setparams((1, 2, rate, 0, "NONE", "not compressed"))
    tone.writeframes((sound * 32767).astype("<i2").tobytes())


def observe_write(connection, payload):
    if len(payload) == 34 and payload.startswith(b"W"):
        frame_counts["waveform"] += 1
        activity["waveform"] |= any(row < ord("i") for row in payload[1:-1])
    elif len(payload) == 18 and payload.startswith(b"F"):
        frame_counts["spectrum"] += 1
        activity["spectrum"] |= any(level > ord("0") for level in payload[1:-1])
    return real_write(connection, payload)


def test_window():
    root = real_tk()
    root.withdraw()
    stage = "desktop"
    deadline = time.monotonic() + 15

    def advance(next_stage):
        nonlocal stage, deadline
        print("PASS", stage, flush=True)
        passed.append(stage)
        stage = next_stage
        deadline = time.monotonic() + 12

    def check():
        try:
            state = api("state")
            if stage == "desktop" and state["desktopConnected"]:
                api("mode", {"mode": "waveform"})
                advance("waveform_ready")
            elif stage == "waveform_ready" and state["audioStreaming"] and state["desktopAudioStatus"] == "ready":
                assert state["mode"] == "waveform" and state["owner"] == "phone"
                winsound.PlaySound(str(tone_path), winsound.SND_FILENAME | winsound.SND_ASYNC)
                advance("waveform_sound")
            elif stage == "waveform_sound" and activity["waveform"]:
                assert state["mode"] == "waveform" and state["owner"] == "phone"
                assert state["audioStreaming"]
                api("mode", {"mode": "spectrum"})
                advance("spectrum_ready")
            elif stage == "spectrum_ready" and state["mode"] == "spectrum" and state["audioStreaming"]:
                assert state["owner"] == "phone"
                winsound.PlaySound(str(tone_path), winsound.SND_FILENAME | winsound.SND_ASYNC)
                advance("spectrum_sound")
            elif stage == "spectrum_sound" and activity["spectrum"]:
                assert state["mode"] == "spectrum" and state["owner"] == "phone"
                assert state["audioStreaming"]
                api("mode", {"mode": "lamp"})
                advance("release_audio")
            elif stage == "release_audio" and state["mode"] == "lamp" and state["desktopAudioStatus"] == "idle":
                assert state["owner"] == "phone" and not state["audioStreaming"]
                api("mode", {"mode": target["mode"]})
                api("brightness", {"value": target["brightness"]})
                advance("saved")
            elif stage == "saved" and state["mode"] == target["mode"] and not state["settingsPending"]:
                advance("done")
                root.destroy()
                return
            if time.monotonic() > deadline:
                raise AssertionError((stage, state, activity, frame_counts))
        except Exception as error:
            errors.append(repr(error))
            root.destroy()
            return
        root.after(150, check)

    root.after(200, check)
    return root


serial.Serial.write = observe_write
tk.Tk = test_window
try:
    app["run"]()
    assert not errors, errors
    assert len(passed) == 7, passed
    offline = api("state")
    assert not offline["desktopConnected"] and not offline["audioStreaming"], offline
    for mode in ("waveform", "spectrum"):
        api("mode", {"mode": mode}, expected=409)
    print("Closing the desktop program disables web audio availability", flush=True)
    print("Real loopback activity:", activity, "transmitted frames:", frame_counts, flush=True)
    print("Web-to-desktop-to-lamp audio checks passed", flush=True)
finally:
    winsound.PlaySound(None, 0)
    api("mode", {"mode": target["mode"]})
    api("brightness", {"value": target["brightness"]})
