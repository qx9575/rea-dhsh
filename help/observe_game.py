"""Requires Python 3 and pip install -U frida. Writes JSONL; Ctrl+C detaches."""
import argparse
import json
from pathlib import Path
import threading
import frida

p = argparse.ArgumentParser()
g = p.add_mutually_exclusive_group()
g.add_argument('--spawn', metavar='EXE', help='Full executable path; normally attach to launcher-started process instead')
g.add_argument('--pid', type=int)
p.add_argument('--log', default='game-observe.jsonl')
a = p.parse_args()
device = frida.get_local_device()
spawned = None
if a.spawn:
    spawned = device.spawn([str(Path(a.spawn).resolve())])
    target = spawned
else:
    target = a.pid if a.pid is not None else 'Game.exe'
session = device.attach(target)
lock = threading.Lock()
with open(a.log, 'a', encoding='utf-8') as f:
    def on_message(message, data):
        record = message.get('payload', message)
        line = json.dumps(record, ensure_ascii=False)
        with lock:
            f.write(line + '\n')
            f.flush()
            print(line, flush=True)
    script = session.create_script(Path(__file__).with_name('game_observe.js').read_text(encoding='utf-8'))
    script.on('message', on_message)
    script.load()
    if spawned is not None:
        device.resume(spawned)
    print('Observing; Ctrl+C to detach. Log: ' + a.log)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            session.detach()
        except frida.InvalidOperationError:
            pass
