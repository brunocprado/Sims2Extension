import struct,sys,collections
def load(path):
    f=open(path,'rb'); hdr=f.read(96)
    maj,minr=struct.unpack('<II',hdr[4:12])
    cnt,off,size,imin=struct.unpack('<I',hdr[36:40])[0],struct.unpack('<I',hdr[40:44])[0],struct.unpack('<I',hdr[44:48])[0],struct.unpack('<I',hdr[60:64])[0]
    f.seek(off); d=f.read(size)
    es=[]; step=24 if imin==2 else 20
    for i in range(cnt):
        t,g,ins=struct.unpack_from('<III',d,i*step)
        if step==24: insh,o,s=struct.unpack_from('<III',d,i*step+12)
        else: insh=0; o,s=struct.unpack_from('<II',d,i*step+12)
        es.append((t,g,ins,insh,o,s))
    return f,es
def qfs(src):
    # RefPack (QFS) decompress: header: 4 byte compressed size, 0x10FB, 3 byte uncompressed size
    sz=struct.unpack_from('<I',src,0)[0]
    assert src[4]==0x10 and src[5]==0xFB
    usz=(src[6]<<16)|(src[7]<<8)|src[8]
    out=bytearray(); i=9
    n=len(src)
    while i<n:
        c=src[i]
        if c>=0xFC:
            plain=c&3; i+=1
            out+=src[i:i+plain]; i+=plain; break
        if c<0x80:
            b1=src[i+1]; plain=c&3; cp=((c&0x1C)>>2)+3; off=((c&0x60)<<3)+b1+1; i+=2
        elif c<0xC0:
            b1=src[i+1]; b2=src[i+2]; plain=(b1>>6)&3; cp=(c&0x3F)+4; off=((b1&0x3F)<<8)+b2+1; i+=3
        elif c<0xE0:
            b1=src[i+1]; b2=src[i+2]; b3=src[i+3]; plain=c&3; cp=((c&0x0C)<<6)+b3+5; off=((c&0x10)<<12)+(b1<<8)+b2+1; i+=4
        else:
            plain=((c&0x1F)<<2)+4; i+=1
            out+=src[i:i+plain]; i+=plain; continue
        out+=src[i:i+plain]; i+=plain
        for _ in range(cp): out.append(out[-off])
    return bytes(out)
def get(f,e):
    t,g,ins,insh,o,s=e
    f.seek(o); b=f.read(s)
    if len(b)>=9 and b[4]==0x10 and b[5]==0xFB: b=qfs(b)
    return b
if __name__=="__main__":
    f,es=load(sys.argv[1])
    h=collections.Counter(e[0] for e in es)
    for t,c in h.most_common(): print("%08X %d"%(t,c))
