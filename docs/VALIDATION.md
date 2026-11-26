# 验证记录 / Validation scope

`checks/results.json` is historical evidence from the local 2026-11-25 single-panel build. It contains no network credentials or private device identifiers.

| 范围 / Scope | 已检查 / Checked | 限制 / Limits |
|---|---|---|
| 原始灯板 / Original panel | 正确目标、Flash hash、中文输入、收藏去重、轮播顺序、模式和亮度恢复。Identity, flash verification, text/favorites, rotation and saved settings. | 用户观察实际字形；不是所有模式逐项重新验收。Physical readability and complete mode-by-mode acceptance are separate. |
| 重启 / Reboot | 软件复位后恢复词句、时长、收藏、模式和亮度。Settings persisted across a software reset. | 该轮未再次进行真实拔电测试。Not a fresh physical cold-power test. |
| 音频基线 / Audio baseline | 实际 Windows 播放音、两种显示、停采、前后台交接、重复启动。Real tones, both modes, capture release and process handoff. | 需要 Windows、播放设备和 USB；1.0 无板载麦克风。Requires a host, not an onboard microphone. |
| 公开版 / Public edition | 单独记录编译、隐私扫描、Python 检查与字库再生成。Build, privacy scan, Python checks and font regeneration are recorded separately. | 未重刷原设备；新的随机密码首次启动路径未在新板上测试。No publication-time flash; first-boot AP generation needs hardware validation. |

## 手动硬件检查 / Manual hardware checks

Read each script before running it. These are integration checks, not CI unit tests. They can change mode, replace test phrases, reset saved test data, play tones or briefly take serial ownership. Keep backups of any custom playlist before the phrase check.

1. Configure `desktop/panel.local.json` with the target MAC.
2. Create `work/` and copy `checks/target.example.json` to `work/web_audio_target.json`; replace the example IP and initial settings with the actual device state.
3. For the background check, start `desktop/音乐律动.pyw --background` first.
4. `check_web_audio.py` expects the audio bridge initially stopped. `check_background_audio.py` expects it running. They are separate scenarios.
5. The phrase prepare/verify check expects the default four-line playlist. It writes a temporary favorite and requires an intervening software reset. The rotation-only phase uses its ignored runtime record.

```powershell
py -3 checks/check_background_audio.py
py -3 checks/check_chinese_panel.py prepare
# Reset the intended controller manually; do not use another device.
py -3 checks/check_chinese_panel.py verify
```

Do not run these checks against an unrelated controller or during another task's device session.
