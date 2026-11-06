"""Check the actual ARM2 rotate helper, including shift-count wrapping."""
import ctypes as C
import subprocess,tempfile,sys
from pathlib import Path
repo=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path(__file__).resolve().parents[4]/'esp32emu'
with tempfile.TemporaryDirectory(prefix='arm2-rotate-') as directory:
    directory=Path(directory)
    core=directory/'core'
    (core/'include').mkdir(parents=True)
    for name in ('arm2.c','arm2_dis.c','include/arm2.h'):
        (core/name).write_bytes(subprocess.check_output(['git','-C',str(repo),'show','8314212:components/arm2/'+name]))
    patch=Path(__file__).resolve().parent/'branchless/change.patch'
    subprocess.run(['patch','--quiet','-p3'],cwd=core,input=patch.read_bytes(),check=True)
    (directory/'wrapper.c').write_text(f'#include "{core / "arm2.c"}"\nuint32_t rotate_checked(uint32_t v, uint32_t n) {{ return ror32(v,n); }}\n')
    library=directory/'core.so'
    subprocess.run(['cc','-O2','-shared','-fPIC','-DEMUDBG_OFF','-fsanitize=undefined','-fno-sanitize-recover=all',
                    '-I'+str(core/'include'),'-I'+str(repo/'components/emudbg/include'),
                    str(directory/'wrapper.c'),str(core/'arm2_dis.c'),'-o',str(library)],check=True)
    check=C.CDLL(str(library)).rotate_checked;check.argtypes=[C.c_uint32,C.c_uint32];check.restype=C.c_uint32
    values={0,0xFFFFFFFF,0xAAAAAAAA,0x55555555,0x12345678,0x87654321}
    values.update(1<<i for i in range(32));values.update(0xFFFFFFFF^(1<<i) for i in range(32))
    counts=list(range(256))+[0x100,0x101,0x7FFFFFFF,0x80000000,0xFFFFFFFF]
    cases=0
    for value in sorted(values):
        for count in counts:
            expected=sum(((value>>bit)&1)<<((bit-count)%32) for bit in range(32))
            assert check(value,count)==expected,(hex(value),hex(count))
            cases+=1
    print(f'{cases} rotate checks pass under UBSan: {len(values)} bit patterns, {len(counts)} counts, independent bit-position oracle')
