"""Check real panel text editing, storage, invalid inputs and timed rotation."""
import json
from pathlib import Path
import runpy
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, ProxyHandler, build_opener

root = Path(__file__).resolve().parents[1]
(root / "work").mkdir(exist_ok=True)
phase = sys.argv[1] if len(sys.argv) > 1 else 'prepare'
if phase == 'rotation':
    ip = json.loads((root / 'work/chinese-panel-check.json').read_text('utf-8'))['stationIp']
else:
    app = runpy.run_path(str(root / 'desktop/音乐律动.pyw'))
    with app['panel_process_lock'](app['FOREGROUND_MUTEX']) as locked:
        assert locked, 'Desktop window is controlling the panel'
        with app['connect_panel'](wait=4) as connection:
            deadline = time.monotonic() + 25
            while True:
                state = app['read_panel_state'](connection)
                if state and state.get('wifiConnected'):
                    break
                if time.monotonic() > deadline:
                    raise AssertionError('Panel Wi-Fi not connected: ' + str(state))
                time.sleep(0.5)
    ip = state['stationIp']
opener = build_opener(ProxyHandler({}))


def api(path, values=None, expected=200):
    body = urlencode(values).encode() if values is not None else None
    request = Request('http://' + ip + '/api/' + path, data=body)
    try:
        with opener.open(request, timeout=5) as response:
            assert expected == response.status
            return json.load(response)
    except HTTPError as error:
        if error.code != expected:
            raise
        return json.load(error)


defaults = ['物来顺应', '未来不迎', '当时不杂', '既往不恋']
record_path = root / 'work/chinese-panel-check.json'
if phase == 'prepare':
    original = api('text')
    initial = api('state')
    print('Panel connected:', ip, 'brightness', initial['brightness'], flush=True)
    assert original['lines'] == defaults and original['seconds'] == 10, original
    edited = {'lines': ['平安喜乐', 'Hello福州'], 'seconds': 3}
    response = api('text', {'config': json.dumps(edited, ensure_ascii=False)})
    assert response['lines'] == edited['lines'] and response['seconds'] == 3
    assert api('state')['mode'] == initial['mode']
    for invalid in [
        {'lines': [], 'seconds': 10},
        {'lines': ['物'] * 17, 'seconds': 10},
        {'lines': ['物'] , 'seconds': 0},
        {'lines': ['物'] , 'seconds': 3.5},
        {'lines': ['物' * 33], 'seconds': 10},
        {'lines': ['开心😀'], 'seconds': 10},
    ]:
        assert api('text', {'config': json.dumps(invalid, ensure_ascii=False)}, 400).get('error')
        assert api('text')['lines'] == edited['lines']
    assert api('text', {'config': '{'}, 400).get('error')
    assert api('text/favorites', {'lines': json.dumps(['物'] * 33)}, 400).get('error')
    assert api('text/favorites', {'lines': json.dumps(['😀'])}, 400).get('error')
    print('PASS replacement and invalid-input handling without changing mode', flush=True)
    temporary = original['favorites'] + ['平安喜乐', '平安喜乐']
    saved = api('text/favorites', {'lines': json.dumps(temporary, ensure_ascii=False)})
    assert saved['favorites'] == list(dict.fromkeys(temporary))
    api('text', {'config': json.dumps({'lines': defaults, 'seconds': 10}, ensure_ascii=False)})
    api('mode', {'mode': 'phrases'})
    time.sleep(2)
    assert not api('state')['settingsPending']
    record = {'original': original, 'expectedFavorites': saved['favorites'],
              'brightness': initial['brightness'], 'stationIp': ip}
    record_path.write_text(json.dumps(record, ensure_ascii=False, indent=2), 'utf-8')
    print('PASS favorites deduplication; ready for reboot persistence check', flush=True)
else:
    record = json.loads(record_path.read_text('utf-8'))
    config, current = api('text'), api('state')
    assert config['lines'] == defaults and config['seconds'] == 10
    assert config['favorites'] == (record['original']['favorites'] if phase == 'rotation' else record['expectedFavorites'])
    assert current['mode'] == 'phrases' and current['brightness'] == record['brightness']
    if phase != 'rotation':
        print('PASS reboot restored playlist, interval, favorites, mode and brightness', flush=True)
    api('text/favorites', {'lines': json.dumps(record['original']['favorites'], ensure_ascii=False)})
    assert api('text')['favorites'] == record['original']['favorites']
    anchor_sent = time.monotonic()
    initial = api('mode', {'mode': 'phrases'})
    started = time.monotonic()
    transitions = []
    previous = None
    previous_sample_sent = started
    while time.monotonic() - started < 41.3:
        sample_sent = time.monotonic()
        current = api('state')
        sample_received = time.monotonic()
        index = current['phraseIndex']
        assert current['mode'] == 'phrases'
        assert current['phraseText'] == defaults[index]
        if index != previous:
            elapsed = time.monotonic() - started
            transitions.append({'index': index, 'seconds': round(elapsed, 2), 'frames': current['frames'],
                                'deviceSeconds': current['uptime'] - initial['uptime'],
                                'observedWindow': [round(previous_sample_sent - started, 3),
                                                   round(sample_received - anchor_sent, 3)]})
            print('Rotation', index + 1, current['phraseText'], round(elapsed, 2), flush=True)
            previous = index
        previous_sample_sent = sample_sent
        time.sleep(0.25)
    assert [item['index'] for item in transitions] == [0, 1, 2, 3, 0], transitions
    for expected_seconds, item in zip([0, 10, 20, 30, 40], transitions):
        # Network delivery does not timestamp the LED transition. Its scheduled
        # boundary must lie between the last old sample and the new reply.
        lower, upper = item['observedWindow']
        assert lower - 0.2 <= expected_seconds <= upper + 0.2, transitions
    assert transitions[-1]['frames'] > transitions[0]['frames'] + 800
    record['transitions'] = transitions
    record['checksPassed'] = True
    record_path.write_text(json.dumps(record, ensure_ascii=False, indent=2), 'utf-8')
    target_path = root / 'work/web_audio_target.json'
    target = json.loads(target_path.read_text('utf-8'))
    target['stationIp'] = ip
    target_path.write_text(json.dumps(target), 'utf-8')
    print('PASS actual panel rotation at ten-second intervals; original favorites restored', flush=True)
