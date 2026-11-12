import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from compile_ignite import compile_project

with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);factory=root/'factory';factory.mkdir()
    for source in (ROOT/'system').glob('*.json'):
        (factory/(source.stem+'.ignit')).write_bytes(compile_project(json.loads(source.read_text(encoding='utf-8')),source.parent))
    for name, appearance, mode in (('dark','dark','normal'), ('light','light','normal'),
                                   ('themed','light','themed')):
        theme=json.loads((ROOT/'system/theme.json').read_text(encoding='utf-8'))
        theme['id']='test.theme-'+name
        theme['components'][0]['id']='main'
        theme['components'][0]['values'].update(appearance=appearance,icons=mode)
        (factory/f'theme-{name}.ignit').write_bytes(compile_project(theme,root))
    # Real icon resources on every app, maximal IDs/components and varied UTF-8 labels.
    project=json.loads((ROOT/'examples/icons/project.json').read_text(encoding='utf-8'))
    project['components'][0]['id']='c'*64
    names=['Ш'*48,'W'*96,'時計'*16,'🕐'*24,'i'*96,'А'*48]
    for index in range(120):
        project['id']=f'app.{index:03d}.'+'x'*56
        project['name']=names[index%len(names)]
        (factory/f'app-{index:03d}.ignit').write_bytes(compile_project(project,ROOT/'examples/icons'))
    output=Path(sys.argv[2]);output.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run([sys.argv[1],str(root/'store'),str(factory),str(output)],check=True,timeout=30)
