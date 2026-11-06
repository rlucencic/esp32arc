"""Check every ARM2 condition, directly and after an in-run PSR write."""
import ctypes as C
import argparse,subprocess,tempfile
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('repo',nargs='?',default=Path(__file__).resolve().parents[4]/'esp32emu')
parser.add_argument('--variant',choices=('constant','eqne'),default='constant')
args=parser.parse_args()
repo=Path(args.repo).resolve()
with tempfile.TemporaryDirectory(prefix='arm2-conditions-') as work:
    root=Path(work)
    core=root/'core'
    (core/'include').mkdir(parents=True)
    for name in ('arm2.c','arm2_dis.c','include/arm2.h'):
        source=subprocess.check_output(['git','-C',str(repo),'show','1acf375:components/arm2/'+name])
        (core/name).write_bytes(source)
    patch=Path(__file__).resolve().parent/args.variant/'change.patch'
    subprocess.run(['patch','--quiet','-p3'],cwd=core,input=patch.read_bytes(),check=True)
    wrapper=subprocess.check_output(['git','-C',str(repo),'show','1acf375:components/arm2/test/diff_lib.c'],text=True)
    wrapper+='\nvoid diff_run(int ticks) { s_cpu.left=0; arm2_run(&s_cpu,ticks); }\n'
    (root/'wrapper.c').write_text(wrapper)
    subprocess.run(['cc','-O2','-shared','-fPIC','-DEMUDBG_OFF','-I'+str(core/'include'),
                    '-I'+str(repo/'components/emudbg/include'),str(root/'wrapper.c'),
                    str(core/'arm2.c'),str(core/'arm2_dis.c'),'-o',str(root/'core.so')],check=True)
    lib=C.CDLL(str(root/'core.so'));lib.diff_init.restype=C.POINTER(C.c_ubyte)
    Regs=C.c_uint32*16
    count=0
    for nzcv in range(16):
        n,z,c,v=[bool(nzcv&bit) for bit in (8,4,2,1)]
        passes=(z,not z,c,not c,n,not n,v,not v,c and not z,not c or z,n==v,n!=v,not z and n==v,z or n!=v,True,False)
        for cond,ok in enumerate(passes):
            for kind,body in [('MOV',0x03A00001),('B',0x0A000000),('BL',0x0B000000)]:
                for preceding in (False,True):
                    mem=lib.diff_init()
                    regs=Regs(*([0]*15),0x1000);regs[1]=nzcv<<28;regs[14]=0x2000
                    code=([0xE331F000] if preceding else [])+[(cond<<28)|body]
                    data=b''.join(op.to_bytes(4,'little') for op in code)
                    C.memmove(C.addressof(mem.contents)+0x1000,data,len(data))
                    lib.diff_set(regs,((nzcv^15) if preceding else nzcv)<<28)
                    lib.diff_run(2 if preceding else 1)
                    out=Regs();flags=C.c_uint32();lib.diff_get(out,C.byref(flags))
                    pc=0x1004 if preceding else 0x1000
                    expected_pc=pc+(8 if ok and kind!='MOV' else 4)
                    assert out[15]==expected_pc,(nzcv,cond,kind,preceding,'PC',hex(out[15]))
                    assert out[0]==int(ok and kind=='MOV'),(nzcv,cond,kind,preceding,'R0')
                    expected_lr=((pc+4)|(nzcv<<28)) if ok and kind=='BL' else 0x2000
                    assert out[14]==expected_lr,(nzcv,cond,kind,preceding,'LR',hex(out[14]),hex(expected_lr))
                    assert flags.value==nzcv<<28,(nzcv,cond,kind,preceding,'flags')
                    count+=1
    print(f'{count} condition cases pass (all NZCV/conditions, MOV/B/BL, direct and after in-run TEQP)')
