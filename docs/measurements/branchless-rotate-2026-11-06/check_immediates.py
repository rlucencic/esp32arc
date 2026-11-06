"""Check all ARM2 immediate values and shifter carries against integer arithmetic."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile,sys
repo=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path(__file__).resolve().parents[4]/'esp32emu'
with tempfile.TemporaryDirectory(prefix='arm2-immediates-') as directory:
    directory=Path(directory)
    core=directory/'core'
    (core/'include').mkdir(parents=True)
    for name in ('arm2.c','arm2_dis.c','include/arm2.h'):
        (core/name).write_bytes(subprocess.check_output(['git','-C',str(repo),'show','8314212:components/arm2/'+name]))
    patch=Path(__file__).resolve().parent/'branchless/change.patch'
    subprocess.run(['patch','--quiet','-p3'],cwd=core,input=patch.read_bytes(),check=True)
    (directory/'wrapper.c').write_text(
        f'#include "{core / "arm2.c"}"\n'
        'uint32_t check_immediate(uint32_t op, uint32_t cin, uint32_t *carry) {\n'
        ' arm2_t cpu = {0}; run_t run = {0}; cpu.psr = cin << 29;\n'
        ' return operand2(&cpu, &run, op, 0, carry, OP2_IMM, 0); }\n')
    library=directory/'core.so'
    subprocess.run(['cc','-O2','-shared','-fPIC','-DEMUDBG_OFF','-I'+str(core/'include'),
                    '-I'+str(repo/'components/emudbg/include'),str(directory/'wrapper.c'),
                    str(core/'arm2_dis.c'),'-o',str(library)],check=True)
    check=C.CDLL(str(library)).check_immediate
    check.argtypes=[C.c_uint32,C.c_uint32,C.POINTER(C.c_uint32)];check.restype=C.c_uint32
    cases=0
    for high in (0,0xE3B00000,0xFFFFFFFF & ~0xFFF):
        for encoded in range(4096):
            rotate=(encoded>>8)*2;byte=encoded&255
            # Rotate a doubled 32-bit bit pattern, then retain its low word.
            doubled=(byte<<32)|byte
            expected=(doubled>>rotate)&0xFFFFFFFF
            for cin in (0,1):
                carry=C.c_uint32(0xDEADBEEF)
                value=check(high|encoded,cin,C.byref(carry))
                expected_carry=(expected>>31) if rotate else cin
                assert (value,carry.value)==(expected,expected_carry),(hex(high|encoded),cin,hex(value),carry.value)
                cases+=1
    print(f'{cases} immediate value/carry checks pass (all 4096 encodings, both input carries, three upper-opcode patterns)')
