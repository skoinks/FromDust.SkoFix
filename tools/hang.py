"""Read a hang dump: python -P -W ignore hang.py file.dmp  -> every thread, with plausible return addresses."""
import struct,sys,os,re
d=open(sys.argv[1],'rb').read()
n,off=struct.unpack_from('<II',d,8)
S={}
for i in range(n):
    t,sz,o=struct.unpack_from('<III',d,off+12*i); S.setdefault(t,(sz,o))
mods=[]
sz,o=S[4]; c=struct.unpack_from('<I',d,o)[0]
for i in range(c):
    b,s,ck,ts,nm=struct.unpack_from('<QIIII',d,o+4+108*i); l=struct.unpack_from('<I',d,nm)[0]
    mods.append((b,s,d[nm+4:nm+4+l].decode('utf-16le').split(chr(92))[-1]))
mem=[]
sz,o=S[5]; c=struct.unpack_from('<I',d,o)[0]
for i in range(c): mem.append(struct.unpack_from('<QII',d,o+4+16*i))
def rd(a,l):
    for s,sz,r in mem:
        if s<=a and a+l<=s+sz: return d[r+a-s:r+a-s+l]
def u32(a):
    b=rd(a,4); return struct.unpack('<I',b)[0] if b else None
imgs={}
G=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','..')   # the game's folder
def mod(a):
    for b,s,nm in mods:
        if b<=a<b+s: return b,s,nm
def name(a):
    m=mod(a); return '%s+%x'%(m[2],a-m[0]) if m else '%x'%a
def img(n):
    if n not in imgs:
        imgs[n]=b''
        for r in (G,G+'/SSE41',G+'/AInative/bin/dll','C:/Windows/SysWOW64'):
            if os.path.exists(r+'/'+n):
                import pefile
                imgs[n]=bytes(pefile.PE(r+'/'+n,fast_load=True).get_memory_mapped_image()); break
    return imgs[n]
def isret(a):
    m=mod(a)
    if not m: return False
    c=img(m[2]); x=a-m[0]
    if x<7 or x>=len(c): return False
    return c[x-5]==0xe8 or (c[x-2]==0xff and c[x-1]&0x38==0x10) or (c[x-3]==0xff and c[x-2]&0x38==0x10) or (c[x-6]==0xff and c[x-5]&0x38==0x10)
def threads():
    sz,o=S[3]; c=struct.unpack_from('<I',d,o)[0]
    for i in range(c):
        tid,sus,pc,pr,teb,sa,ss,sr,cs,cr=struct.unpack_from('<IIIIQQIIII',d,o+4+48*i)
        r={k:struct.unpack_from('<I',d,cr+x)[0] for k,x in dict(edi=0x9c,esi=0xa0,ebx=0xa4,edx=0xa8,ecx=0xac,eax=0xb0,ebp=0xb4,eip=0xb8,esp=0xc4).items()}
        yield i,tid,pr-(1<<32) if pr>>31 else pr,r,sa,ss
if __name__=='__main__':
    for i,tid,pr,r,sa,ss in threads():
        b=rd(r['esp'],sa+ss-r['esp']) or b''
        L=[]
        for k in range(0,len(b)-3,4):
            v=struct.unpack_from('<I',b,k)[0]
            if v>0x400000 and isret(v): L.append(name(v))
            if len(L)>24: break
        print('T%d tid %d prio %d eip %s: %s'%(i,tid,pr,name(r['eip']),' '.join(L)))
