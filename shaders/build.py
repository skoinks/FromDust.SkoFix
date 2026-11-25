# Compiles water.hlsl into src/water_shaders.h (and the plain versions for tools/watertest.cpp).
import glob, os, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
fxc = sorted(glob.glob(r'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe'))[-1]
out = '// Made by shaders/build.py from water.hlsl.\n#pragma once\n#include <windows.h>\n'
for name, entry, target, enhanced in (('g_waterDeferred', 'Deferred', 'ps_3_0', 1), ('g_waterForward', 'Forward', 'ps_3_0', 1),
                                      ('g_waterForwardVS', 'ForwardVS', 'vs_3_0', 1),
                                      ('g_plainDeferred', 'Deferred', 'ps_3_0', 0), ('g_plainForward', 'Forward', 'ps_3_0', 0)):
    b = os.path.join(here, '..', 'build', name + '.cso')
    asm = b[:-4] + '.asm'
    r = subprocess.run([fxc, '/nologo', '/O3', '/T', target, '/E', entry, '/D', 'ENHANCED=%d' % enhanced, '/Fo', b, '/Fc', asm,
                        os.path.join(here, 'water.hlsl')], capture_output=True, text=True)
    print(name, r.returncode, (r.stdout + r.stderr).strip()[-900:])
    if r.returncode:
        sys.exit(1)
    print([x.strip() for x in open(asm) if 'approximately' in x])
    if enhanced:
        d = open(b, 'rb').read()
        out += 'alignas(4) static const BYTE %s[] = {\n' % name
        for i in range(0, len(d), 32):
            out += ','.join(map(str, d[i:i + 32])) + ',\n'
        out += '};\n'
open(os.path.join(here, '..', 'src', 'water_shaders.h'), 'w').write(out)
