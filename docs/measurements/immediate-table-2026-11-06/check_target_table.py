"""Verify the immediate table in a linked P4 ELF (requires pyelftools)."""
import hashlib,json,struct,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
path=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[3]/'build-tab5/esp32arc.elf'
with path.open('rb') as stream:
    elf=ELFFile(stream)
    symbol=elf.get_section_by_name('.symtab').get_symbol_by_name('s_immediate')[0]
    section=elf.get_section(symbol['st_shndx'])
    start=symbol['st_value']-section['sh_addr']
    data=section.data()[start:start+symbol['st_size']]
    expected=[]
    for encoded in range(4096):
        byte=encoded&255;rotate=(encoded>>8)*2
        expected.append((((byte<<32)|byte)>>rotate)&0xFFFFFFFF)
    assert struct.unpack('<4096I',data)==tuple(expected)
    assert section.name=='.dram1.data',section.name
    print(json.dumps({'symbol':'s_immediate','address':hex(symbol['st_value']),
                      'section':section.name,'bytes':len(data),'entries_verified':4096,
                      'sha256':hashlib.sha256(data).hexdigest()},indent=2))
