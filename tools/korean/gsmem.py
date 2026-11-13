"""PS2 GS swizzle: PSMCT32 upload vs PSMT4 read (tables from PCSX2 GSTables.cpp)."""
import struct, zlib
BT32=[[0,1,4,5,16,17,20,21],[2,3,6,7,18,19,22,23],[8,9,12,13,24,25,28,29],[10,11,14,15,26,27,30,31]]
BT4=[[0,2,8,10],[1,3,9,11],[4,6,12,14],[5,7,13,15],[16,18,24,26],[17,19,25,27],[20,22,28,30],[21,23,29,31]]
CT32=[[0,1,4,5,8,9,12,13],[2,3,6,7,10,11,14,15],[16,17,20,21,24,25,28,29],[18,19,22,23,26,27,30,31],[32,33,36,37,40,41,44,45],[34,35,38,39,42,43,46,47],[48,49,52,53,56,57,60,61],[50,51,54,55,58,59,62,63]]
C4=[[0,8,32,40,64,72,96,104,2,10,34,42,66,74,98,106,4,12,36,44,68,76,100,108,6,14,38,46,70,78,102,110],
[16,24,48,56,80,88,112,120,18,26,50,58,82,90,114,122,20,28,52,60,84,92,116,124,22,30,54,62,86,94,118,126],
[65,73,97,105,1,9,33,41,67,75,99,107,3,11,35,43,69,77,101,109,5,13,37,45,71,79,103,111,7,15,39,47],
[81,89,113,121,17,25,49,57,83,91,115,123,19,27,51,59,85,93,117,125,21,29,53,61,87,95,119,127,23,31,55,63]]
def ct4(y,x):
    col=(y%16)//4; r=y%4
    base=[0,128,256,384][col]
    row=C4[r] if col%2==0 else None
    if col%2==0: v=C4[r][x]
    else: v=[[192,200,224,232,128,136,160,168,194,202,226,234,130,138,162,170,196,204,228,236,132,140,164,172,198,206,230,238,134,142,166,174],
             [208,216,240,248,144,152,176,184,210,218,242,250,146,154,178,186,212,220,244,252,148,156,180,188,214,222,246,254,150,158,182,190],
             [129,137,161,169,193,201,225,233,131,139,163,171,195,203,227,235,133,141,165,173,197,205,229,237,135,143,167,175,199,207,231,239],
             [145,153,177,185,209,217,241,249,147,155,179,187,211,219,243,251,149,157,181,189,213,221,245,253,151,159,183,191,215,223,247,255]][r][x]-128
    return base+v
def addr32(x,y,bw):  # word address
    page=(y//32)*bw+(x//64); return page*2048+BT32[(y%32)//8][(x%64)//8]*64+CT32[y%8][x%8]
def addr4(x,y,tbw):  # nibble address
    page=(y//128)*(tbw//2)+(x//128); return page*16384+BT4[(y%128)//16][(x%128)//32]*512+ct4(y%16,x%32)
def unswizzle_t4_from_ct32(data,w4,h4):
    """data: 4bpp texture stored as a PSMCT32 image (w4/2 x h4/4) for upload; returns rows of nibbles."""
    w32,h32=w4//2,h4//4; bw=w32//64; tbw=w4//64
    mem=bytearray(len(data))
    words=struct.unpack('<%dI'%(len(data)//4),data)
    for y in range(h32):
        for x in range(w32):
            struct.pack_into('<I',mem,addr32(x,y,bw)*4,words[y*w32+x])
    rows=[]
    for y in range(h4):
        row=bytearray(w4)
        for x in range(w4):
            n=addr4(x,y,tbw); b=mem[n>>1]; row[x]=(b>>4) if n&1 else (b&15)
        rows.append(row)
    return rows
def png(path,rows):
    h=len(rows); w=len(rows[0]); raw=b''.join(b'\x00'+bytes(r) for r in rows)
    def ch(t,d): c=struct.pack('>I',len(d))+t+d; return c+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,0,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))


def swizzle_t4_to_ct32(rows, w4, h4):
    """Inverse of unswizzle_t4_from_ct32: nibble rows -> bytes of the PSMCT32 image (w4/2 x h4/4, row-major)."""
    w32, h32 = w4 // 2, h4 // 4
    bw = w32 // 64
    tbw = w4 // 64
    mem = bytearray(w4 * h4 // 2)
    for y in range(h4):
        r = rows[y]
        for x in range(w4):
            n = addr4(x, y, tbw)
            i = n >> 1
            if n & 1:
                mem[i] = (mem[i] & 0x0F) | (r[x] << 4)
            else:
                mem[i] = (mem[i] & 0xF0) | r[x]
    out = bytearray(len(mem))
    for y in range(h32):
        for x in range(w32):
            a = addr32(x, y, bw) * 4
            o = (y * w32 + x) * 4
            out[o:o + 4] = mem[a:a + 4]
    return bytes(out)
