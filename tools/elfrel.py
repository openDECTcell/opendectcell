# SPDX-License-Identifier: GPL-2.0-only
"""List pointer tables in a 32-bit relocatable ELF (ARM REL / MIPS REL): for each data section,
resolve ABS32 relocations to strings/symbols and print entries with the raw words in between."""
import struct,sys,re
d=open(sys.argv[1],'rb').read()
E='<' if d[5]==1 else '>'
shoff,=struct.unpack_from(E+'I',d,0x20); shentsize,shnum,shstrndx=struct.unpack_from(E+'HHH',d,0x2e)
secs=[struct.unpack_from(E+'IIIIIIIIII',d,shoff+i*shentsize) for i in range(shnum)]
def sname(i): o=secs[shstrndx][4]+secs[i][0]; return d[o:d.index(b'\0',o)].decode()
names=[sname(i) for i in range(shnum)]
symtab=[i for i,s in enumerate(secs) if s[1]==2][0]
st=secs[symtab]; strt=secs[st[6]]
syms=[]
for j in range(st[5]//16):
    nm,val,sz,info,oth,shn=struct.unpack_from(E+'IIIBBH',d,st[4]+16*j)
    o=strt[4]+nm; syms.append((d[o:d.index(b'\0',o)].decode(),val,shn,info&0xf))
def cstr(sec,off):
    o=secs[sec][4]+off
    if o>=len(d) or secs[sec][1]==8: return None
    e=d.find(b'\0',o); s=d[o:e]
    return s.decode('latin1') if s and all(32<=c<127 for c in s) else None
want=re.compile(sys.argv[2]) if len(sys.argv)>2 else None
for i,s in enumerate(secs):
    if s[1]!=9: continue  # SHT_REL
    tgt=s[7]; tname=names[tgt]
    if not tname.startswith(('.data','.rodata')): continue
    rows=[]
    for k in range(s[5]//8):
        off,info=struct.unpack_from(E+'II',d,s[4]+8*k)
        sym=syms[info>>8]; add,=struct.unpack_from(E+'I',d,secs[tgt][4]+off)
        if sym[0]=='' and sym[3]==3:   # section symbol
            txt=cstr(sym[2],add)
        else:
            txt='&'+sym[0]+('+%d'%add if add else '')
        rows.append((off,txt))
    rows.sort()
    for off,txt in rows:
        if txt and (want is None or want.search(txt)):
            # owning data symbol
            own=[x for x in syms if x[2]==tgt and x[1]<=off and x[3]==1]
            own=max(own,key=lambda x:x[1]) if own else ('?',0)
            nxt=struct.unpack_from(E+'I',d,secs[tgt][4]+off+4)[0] if off+8<=secs[tgt][2] else None
            prv=struct.unpack_from(E+'I',d,secs[tgt][4]+off-4)[0] if off>=4 else None
            print(f"{tname}+{off:#x}\t{own[0]}+{off-own[1]:#x}\tprev={prv:#x} next={nxt:#x}\t{txt}" if prv is not None and nxt is not None else f"{tname}+{off:#x}\t{txt}")
