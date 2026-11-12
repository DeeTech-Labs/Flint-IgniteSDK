import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from flim import encode, load_image
from compile_ignite import compile_project
from build_package import build, TYPES


class ConverterTests(unittest.TestCase):
    def test_alpha_and_odd_mask(self):
        image = Image.new("RGBA", (3, 1))
        image.putdata([(250, 0, 0, 0), (0, 255, 0, 128), (0, 0, 255, 255)])
        data, result = encode(image, "A4", "alpha")
        self.assertEqual(data[16:], bytes([0x08, 0xf0]))
        self.assertEqual(list(result.getchannel("A").getdata()), [0, 136, 255])
        with self.assertRaises(ValueError): encode(image, "A4")
        data, preview = encode(image, "RGB565+A8")
        self.assertEqual([data[i] for i in (18, 21, 24)], [0, 128, 255])
        self.assertEqual(preview.getpixel((2, 0)), (0, 0, 255, 255))

    def test_palette_combinations_and_quantization(self):
        image = Image.new("RGBA", (256, 1))
        image.putdata([(255, 0, 0, a) for a in range(256)])
        data, _ = encode(image, "INDEX8")
        self.assertEqual(struct.unpack_from("<H", data, 10)[0], 256)
        reduced, _ = encode(image, "INDEX8", colors=16)
        self.assertLessEqual(struct.unpack_from("<H", reduced, 10)[0], 16)
        for n in (1, 2, 16, 255, 256):
            data, _ = encode(Image.new("RGBA", (48,48), (255,0,0,255)), "INDEX8", colors=n)
            self.assertEqual(len(data), 16+3+48*48)
        for n in (0,257):
            with self.assertRaises(ValueError): encode(image,"INDEX8",colors=n)

    def test_host_scaling_and_svg_references(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            image=Image.new("RGBA",(128,128),(255,0,0,0))
            image.putpixel((64,64),(0,255,0,255));image.save(root/"input.png")
            self.assertEqual(load_image(root/"input.png",[48,48]).size,(48,48))
            svg=root/"bad.svg"
            svg.write_text('<svg xmlns="http://www.w3.org/2000/svg"><image href="file:///secret"/></svg>')
            with self.assertRaises(ValueError): load_image(svg,[48,48])

    def test_optional_svg_backend(self):
        try:
            from cairosvg.surface import PNGSurface
        except (ImportError, OSError):
            self.skipTest("optional host CairoSVG/Cairo backend unavailable")
        with tempfile.TemporaryDirectory() as folder:
            svg = Path(folder)/"vector.svg"
            svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48">'
                           '<circle cx="24" cy="24" r="20" fill="red" opacity="0.5"/></svg>')
            image = load_image(svg,[48,48])
            self.assertEqual(image.size,(48,48))
            self.assertEqual(image.getpixel((0,0))[3],0)
            self.assertIn(image.getpixel((24,24))[3],(127,128))


def integration(executable):
    with tempfile.TemporaryDirectory() as directory:
        root=Path(directory);(root/"factory").mkdir();(root/"store").mkdir()
        project={'id':'test.icons','publisher':'tests','name':'Icons','version':1,
            'permissions':['ui.render'],'icons':{'main':100,'mask':101,'light':102,'dark':103},
            'components':[{'id':'main','type':'app','ui':[{'kind':'rect','w':280,'h':240}]}],
            'resources':[]}
        for index,(color,fmt) in enumerate((((255,0,0,255),'INDEX8'),((255,255,255,255),'A4'),
                                         ((0,255,0,255),'INDEX8'),((0,0,255,255),'RGB565+A8'))):
            data,_=encode(Image.new('RGBA',(48,48),color),fmt,'alpha' if fmt=='A4' else None)
            (root/f'{100+index}.flim').write_bytes(data)
            project['resources'].append({'id':100+index,'path':f'{100+index}.flim'})
        (root/'variants.ignit').write_bytes(compile_project(project,root))
        project['icons']={'main':100};(root/'main.ignit').write_bytes(compile_project(project,root))
        project['version']=2;project['icons']={'main':103}
        updated=compile_project(project,root);(root/'updated.ignit').write_bytes(updated)
        broken=bytearray(updated);broken[-1]^=1;(root/'integrity.ignit').write_bytes(broken)
        # Bypass the authoring checks to exercise signed-length-correct but malformed resources.
        def corrupt(name,payload):
            sections=[]
            for i in range(struct.unpack_from('<I',updated,20)[0]):
                ident,kind,_,offset,size,_,_=struct.unpack_from('<IHHIIII',updated,64+24*i)
                sections.append((ident,{v:k for k,v in TYPES.items()}[kind],payload if ident==103 else updated[offset:offset+size]))
            (root/name).write_bytes(build(sections))
        corrupt('truncated.ignit',b'FLIM')
        invalid,_=encode(Image.new('RGBA',(48,48),(255,0,0,255)),'INDEX8')
        invalid=bytearray(invalid);invalid[-1]=1;corrupt('bad-index.ignit',invalid)
        invalid,_=encode(Image.new('RGBA',(48,48),(255,255,255,255)),'A4','alpha')
        corrupt('bad-type.ignit',invalid)
        project.pop('icons');(root/'missing.ignit').write_bytes(compile_project(project,root))
        subprocess.run([executable,str(root)],check=True)


if __name__=='__main__':
    executable=sys.argv.pop(1)
    integration(executable)
    unittest.main()
