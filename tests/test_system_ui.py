import json
import subprocess
import sys
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from compile_ignite import compile_project
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    factory = root / 'factory'
    factory.mkdir()
    for source in (ROOT / 'system').glob('*.json'):
        (factory / (source.stem + '.ignit')).write_bytes(compile_project(
            json.loads(source.read_text(encoding='utf-8')), source.parent))
    subprocess.run([sys.argv[1], str(root / 'store'), str(factory), sys.argv[2]], check=True)

    payload = {
        'id': 'test.ui-payload', 'name': 'Test application', 'publisher': 'tests', 'version': 1,
        'permissions': [{'id': 'companion.pc.metrics', 'scope': 'cpu,ram'}, 'ui.render'],
        'components': [{'id': 'main', 'type': 'app', 'ui': [
            {'kind': 'rect', 'w': 280, 'h': 240, 'color': '#000000'}]}],
    }
    (root / 'payload.ignit').write_bytes(compile_project(payload, root))
    fault = json.loads((ROOT / 'examples/counter/project.json').read_text(encoding='utf-8'))
    fault['id'] = 'test.ui-fault'
    fault['permissions'] = ['ui.render']
    (root / 'main.lua').write_text('return {update=function(dt) if dt>0 then while true do end end end}')
    (root / 'fault.ignit').write_bytes(compile_project(fault, root))
    subprocess.run([sys.argv[3], str(root), str(factory),
                    str(Path(sys.argv[2]) / 'states')], check=True)

    # Exercise maximal UTF-8 names and a catalogue larger than the visible four rows.
    for index, name in enumerate(('Ш' * 48, 'W' * 96, '時計' * 16, '🕐' * 24, 'A')):
        project = {
            'id': f'a.stress-{index}', 'name': name, 'publisher': 'tests', 'version': 1,
            'permissions': ['ui.render'],
            'components': [{'id': 'main', 'type': 'app', 'ui': [
                {'kind': 'rect', 'w': 280, 'h': 240, 'color': '#000000'}]}],
        }
        (factory / f'stress-{index}.ignit').write_bytes(compile_project(project, root))
    subprocess.run([sys.argv[1], str(root / 'stress-store'), str(factory),
                    str(Path(sys.argv[2]) / 'stress')], check=True)

    # Launcher is excluded from its own catalogue; a widget clock and no Settings yield zero apps.
    empty = root / 'empty-factory'
    empty.mkdir()
    for source in (ROOT / 'system').glob('*.json'):
        if source.stem == 'settings':
            continue
        project = json.loads(source.read_text(encoding='utf-8'))
        if source.stem == 'clock':
            project['components'][0]['type'] = 'widget'
        (empty / (source.stem + '.ignit')).write_bytes(compile_project(project, source.parent))
    subprocess.run([sys.argv[1], str(root / 'empty-store'), str(empty),
                    str(Path(sys.argv[2]) / 'empty')], check=True)
