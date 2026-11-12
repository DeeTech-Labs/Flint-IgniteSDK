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
    for index in range(12):
        source={'id':f'test.tile-{index:02d}','name':f'Application {index+1}','publisher':'tests','version':1,'permissions':['ui.render'],'components':[{'id':'main','type':'app','ui':[{'kind':'rect','w':280,'h':240,'color':'#000000'}]}]}
        (factory/f'zz-{index:02d}.ignit').write_bytes(compile_project(source,root))
    # A user package may claim a Settings role, but it cannot inherit factory authority.
    (root/'hostile.lua').write_text("return {update=function(dt) if dt==0 then ignite.value('initial',ignite.action('role','settings'));ignite.schedule(0) end end,event=function(k) if k=='touch_up' then ignite.value('result',ignite.action('system','ble_toggle')) end end}")
    hostile={'id':'test.hostile','name':'Hostile','publisher':'tests','version':1,'app_api':1,'capabilities':6337,
        'budget':{'internal':49152,'external':0,'instructions':4096},
        'permissions':['ui.render','input.focused','ui.navigation','time.timer.create'],
        'logic':{'language':'lua','path':'hostile.lua'},
        'components':[{'id':'main','type':'app','role':'settings','ui':[
            {'kind':'rect','w':280,'h':240,'color':'#000000'},
            {'kind':'text','x':20,'y':70,'text':'Init:${app.initial}; Action:${app.result}','color':'#ffffff'}]}]}
    (root/'hostile.ignit').write_bytes(compile_project(hostile,root))
    subprocess.run([sys.argv[1],str(root/'store'),str(factory),sys.argv[2]],check=True)
