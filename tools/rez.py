#!/usr/bin/env python3
"""LithTech REZ archive lister/extractor. usage: rez.py <file.rez> [outdir]"""
import struct,sys,os
HDR=2+60+2+60+3
def walk(f,pos,size,path,out):
    f.seek(pos); data=f.read(size); i=0
    while i<len(data):
        typ,=struct.unpack_from('<I',data,i); i+=4
        if typ==1:
            p,s,t=struct.unpack_from('<III',data,i); i+=12
            e=data.index(b'\0',i); name=data[i:e].decode('latin1'); i=e+1
            walk(f,p,s,path+name+'/',out)
        else:
            p,s,t,rid=struct.unpack_from('<IIII',data,i); i+=16
            ext=data[i:i+4][::-1].strip(b'\0 ').decode('latin1'); i+=4
            nk,=struct.unpack_from('<I',data,i); i+=4
            e=data.index(b'\0',i); name=data[i:e].decode('latin1'); i=e+1
            e=data.index(b'\0',i); i=e+1
            i+=4*nk
            fn=path+name+('.'+ext if ext else '')
            out.append((fn,p,s))
def lst(fp):
    f=open(fp,'rb'); f.seek(HDR)
    v,rp,rs=struct.unpack('<III',f.read(12))
    out=[]; walk(f,rp,rs,'',out); return f,out
if __name__=='__main__':
    f,out=lst(sys.argv[1])
    if len(sys.argv)>2:
        for fn,p,s in out:
            d=os.path.join(sys.argv[2],fn); os.makedirs(os.path.dirname(d),exist_ok=True)
            f.seek(p); open(d,'wb').write(f.read(s))
    for fn,p,s in out: print(s,fn)
