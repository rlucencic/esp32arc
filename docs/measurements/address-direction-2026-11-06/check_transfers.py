"""Compare candidate and committed ARM2 cores for single transfers."""
import ctypes as C
import itertools
import subprocess
import tempfile
import sys
from pathlib import Path
root=Path(__file__).resolve().parent
repo=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else root.parents[3]/'esp32emu'
work=tempfile.TemporaryDirectory(prefix='arm2-transfers-')
workspace=Path(work.name)
source=subprocess.check_output(['git','-C',str(repo),'show','96fd93c:components/arm2/arm2.c'])
libs=[]
for name in ('baseline','direction'):
    directory=workspace/name
    directory.mkdir()
    (directory/'arm2.c').write_bytes(source)
    if name=='direction':
        subprocess.run(['patch','--quiet','-p3'],cwd=directory,
                       input=(root/'direction/change.patch').read_bytes(),check=True)
    libpath=directory/'libarm2.so'
    wrapper=(repo/'components/arm2/test/diff_lib.c').read_text()
    wrapper+='\nint diff_ticks(void) { return s_cpu.left; }\nuint32_t diff_psr(void) { return s_cpu.psr; }\n'
    (directory/'diff_lib.c').write_text(wrapper)
    subprocess.run(['cc','-O2','-shared','-fPIC','-DEMUDBG_OFF',
                    '-I'+str(repo/'components/arm2/include'),
                    '-I'+str(repo/'components/emudbg/include'),
                    str(directory/'diff_lib.c'),str(directory/'arm2.c'),
                    str(repo/'components/arm2/arm2_dis.c'),'-o',str(libpath)],check=True)
    libs.append(C.CDLL(str(libpath)))
for lib in libs:
    lib.diff_init.restype=C.POINTER(C.c_ubyte)
Regs=C.c_uint32*16
initial=bytes(range(256))*256
count=0
for load,byte,pre,up,wb,reg,rn,rd,align in itertools.product(range(2),range(2),range(2),range(2),range(2),range(2),range(16),range(16),range(4)):
    op=(0xE4000000|(reg<<25)|(pre<<24)|(up<<23)|(byte<<22)|(wb<<21)|(load<<20)|(rn<<16)|(rd<<12))
    # Exercise plain and shifted Rm, with aliases allowed, as well as immediates.
    op|=(6|((align&1)<<7)) if reg else 0x40
    regs=Regs(*[0x100+i*4 for i in range(15)],0x1000)
    if rn!=15:regs[rn]=0x6080+align
    results=[]
    for lib in libs:
        mem=lib.diff_init()
        C.memmove(mem,initial,len(initial))
        C.memmove(C.addressof(mem.contents)+0x1000,op.to_bytes(4,'little'),4)
        lib.diff_set(regs,0xA0000000)
        exception=lib.diff_step()
        out=Regs();flags=C.c_uint32()
        lib.diff_get(out,C.byref(flags))
        results.append((exception,bytes(out),flags.value,lib.diff_psr(),lib.diff_ticks(),C.string_at(mem,65536)))
    assert results[0]==results[1],(load,byte,pre,up,wb,reg,rn,rd,align,hex(op))
    count+=1
print(f'{count} single-transfer cases agree with committed core (registers, PC, PSR, cycles, exception, all RAM)')
