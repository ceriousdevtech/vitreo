# Generates the GDI+ codec fixtures (tiny, exact colours).
import struct, sys, os, io
from PIL import Image
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
W=H=4
cols=[(255,0,0),(0,255,0),(0,0,255),(255,255,255)]
def rgb_img():
    im=Image.new('RGB',(W,H))
    for y in range(H):
        for x in range(W): im.putpixel((x,y), cols[(x+y)%4])
    return im
def rgba_img():
    im=Image.new('RGBA',(W,H))
    for y in range(H):
        for x in range(W):
            c=cols[(x+y)%4]; im.putpixel((x,y), c+((255,128,0,255)[x],))
    return im
rgb=rgb_img(); rgba=rgba_img()
rgb.save(f'{out}/rgb24.bmp')
rgba.save(f'{out}/argb32.bmp')
rgb.convert('P', palette=Image.ADAPTIVE, colors=4).save(f'{out}/pal8.bmp')
rgb.convert('1').save(f'{out}/mono1.bmp')
# 4bpp BMP by hand: palette 16 entries, pixel index (x+y)%4
def bmp4():
    row=b''
    rows=[]
    for y in range(H-1,-1,-1):
        r=bytes([(((x+y)%4)<<4)|((x+1+y)%4) for x in range(0,W,2)])
        r+=b'\0'*((4-len(r)%4)%4); rows.append(r)
    pix=b''.join(rows)
    pal=b''.join(bytes([b,g,r_,0]) for (r_,g,b) in cols)+b'\0'*4*12
    off=14+40+len(pal)
    hdr=b'BM'+struct.pack('<IHHI',off+len(pix),0,0,off)
    ih=struct.pack('<IiiHHIIiiII',40,W,H,1,4,0,len(pix),2835,2835,16,0)
    return hdr+ih+pal+pix
open(f'{out}/pal4.bmp','wb').write(bmp4())
def bmp16():
    rows=[]
    for y in range(H-1,-1,-1):
        r=b''
        for x in range(W):
            R,G,B=cols[(x+y)%4]; r+=struct.pack('<H',((R>>3)<<10)|((G>>3)<<5)|(B>>3))
        rows.append(r)
    pix=b''.join(rows); off=54
    return b'BM'+struct.pack('<IHHI',off+len(pix),0,0,off)+struct.pack('<IiiHHIIiiII',40,W,H,1,16,0,len(pix),3780,3780,0,0)+pix
open(f'{out}/rgb555.bmp','wb').write(bmp16())
rgb.save(f'{out}/rgb24.png'); rgba.save(f'{out}/rgba32.png')
rgb.convert('P', palette=Image.ADAPTIVE, colors=4).save(f'{out}/pal8.png')
rgb.convert('P', palette=Image.ADAPTIVE, colors=4).save(f'{out}/pal2bit.png', bits=2)
rgb.convert('L').save(f'{out}/gray8.png')
rgba.convert('LA').save(f'{out}/graya.png')
Image.new('I;16',(W,H)).save(f'{out}/gray16.png')
# png with gamma/phys/text via pnginfo
from PIL import PngImagePlugin
pi=PngImagePlugin.PngInfo(); pi.add_text('Title','Vitreo'); pi.add_text('Author','probe')
rgb.save(f'{out}/meta.png', pnginfo=pi, dpi=(72,72))
im=rgb.copy(); im.info['gamma']=0.45455
rgb.save(f'{out}/rgb24.jpg', quality=95)
rgb.convert('L').save(f'{out}/gray.jpg', quality=95)
exif=Image.Exif(); exif[0x010F]='VitreoCam'; exif[0x0110]='Model 1'; exif[0x0112]=1; exif[0x0132]='2026:10:27 12:00:00'
exif[0x011A]=(72,1); exif[0x011B]=(72,1); exif[0x0128]=2
rgb.save(f'{out}/exif.jpg', quality=90, exif=exif.tobytes(), dpi=(72,72))
p=rgb.convert('P', palette=Image.ADAPTIVE, colors=4)
p.save(f'{out}/single.gif', transparency=0)
f1=rgb.convert('P', palette=Image.ADAPTIVE, colors=4); f2=rgb.transpose(Image.FLIP_LEFT_RIGHT).convert('P', palette=Image.ADAPTIVE, colors=4)
f1.save(f'{out}/anim.gif', save_all=True, append_images=[f2], duration=[100,250], loop=3)
rgb.save(f'{out}/two.tif', save_all=True, append_images=[rgb.transpose(Image.FLIP_LEFT_RIGHT)])
rgba.save(f'{out}/rgba.tif')
rgba.save(f'{out}/icon.ico', sizes=[(4,4)])
# ---- round 2 ----
import zlib
def png_chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
def png(ihdr, rows, extra=b''):
    raw = b''.join(b'\0' + r for r in rows)
    return b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', ihdr) + extra + png_chunk(b'IDAT', zlib.compress(raw)) + png_chunk(b'IEND', b'')
def ihdr(bd, ct): return struct.pack('>IIBBBBB', W, H, bd, ct, 0, 0, 0)
rows48 = [b''.join(struct.pack('>HHH', *(v * 257 for v in cols[(x + y) % 4])) for x in range(W)) for y in range(H)]
open(f'{out}/rgb48.png', 'wb').write(png(ihdr(16, 2), rows48))
rows64 = [b''.join(struct.pack('>HHHH', *([v * 257 for v in cols[(x + y) % 4]] + [(65535, 32768, 0, 65535)[x]])) for x in range(W)) for y in range(H)]
open(f'{out}/rgba64.png', 'wb').write(png(ihdr(16, 6), rows64))
pal = b''.join(bytes(c) for c in cols)
rowsp = [bytes([(x + y) % 4 for x in range(W)]) for y in range(H)]
open(f'{out}/paltrns.png', 'wb').write(png(ihdr(8, 3), rowsp, png_chunk(b'PLTE', pal) + png_chunk(b'tRNS', b'\xff\x80')))
txt = b''
for k, v in [(b'Description', b'desc'), (b'Copyright', b'copy'), (b'Software', b'soft'), (b'Creation Time', b'ctime'),
             (b'Source', b'src'), (b'Comment', b'comm'), (b'Disclaimer', b'disc'), (b'Warning', b'warn'), (b'Other', b'oth')]:
    txt += png_chunk(b'tEXt', k + b'\0' + v)
chunks = (png_chunk(b'gAMA', struct.pack('>I', 45455)) + png_chunk(b'cHRM', struct.pack('>8I', 31270, 32900, 64000, 33000, 30000, 60000, 15000, 6000))
          + png_chunk(b'sRGB', b'\x01') + png_chunk(b'iCCP', b'prof\0\0' + zlib.compress(b'ICCDATA!')) + png_chunk(b'tIME', struct.pack('>HBBBBB', 2026, 10, 27, 13, 14, 15))
          + png_chunk(b'pHYs', struct.pack('>IIB', 1000, 2000, 0)) + txt + png_chunk(b'zTXt', b'Title\0\0' + zlib.compress(b'ztitle')))
rows24 = [b''.join(bytes(cols[(x + y) % 4]) for x in range(W)) for y in range(H)]
open(f'{out}/chunks.png', 'wb').write(png(ihdr(8, 2), rows24, chunks))
# BMP variants
def bmp_file(bih, pal, pix): off = 14 + len(bih) + len(pal); return b'BM' + struct.pack('<IHHI', off + len(pix), 0, 0, off) + bih + pal + pix
pix24 = b''.join(b''.join(bytes(cols[(x + y) % 4][::-1]) for x in range(W)) for y in range(H - 1, -1, -1))
open(f'{out}/noppm.bmp', 'wb').write(bmp_file(struct.pack('<IiiHHIIiiII', 40, W, H, 1, 24, 0, len(pix24), 0, 0, 0, 0), b'', pix24))
pixtd = b''.join(b''.join(bytes(cols[(x + y) % 4][::-1]) for x in range(W)) for y in range(H))
open(f'{out}/topdown.bmp', 'wb').write(bmp_file(struct.pack('<IiiHHIIiiII', 40, W, -H, 1, 24, 0, len(pixtd), 0, 0, 0, 0), b'', pixtd))
pix32 = b''.join(b''.join(bytes(cols[(x + y) % 4][::-1]) + bytes([(255, 128, 0, 255)[x]]) for x in range(W)) for y in range(H - 1, -1, -1))
v5 = struct.pack('<IiiHHIIiiII', 124, W, H, 1, 32, 3, len(pix32), 3780, 3780, 0, 0) + struct.pack('<IIII', 0xff0000, 0xff00, 0xff, 0xff000000) + b'BGRs' + b'\0' * 36 + b'\0' * 12 + struct.pack('<IIII', 4, 0, 0, 0)
open(f'{out}/v5alpha.bmp', 'wb').write(bmp_file(v5, b'', pix32))
pix565 = b''.join(b''.join(struct.pack('<H', ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)) for c in [cols[(x + y) % 4] for x in range(W)]) for y in range(H - 1, -1, -1))
open(f'{out}/rgb565.bmp', 'wb').write(bmp_file(struct.pack('<IiiHHIIiiII', 40, W, H, 1, 16, 3, len(pix565), 3780, 3780, 0, 0) + struct.pack('<III', 0xf800, 0x7e0, 0x1f), b'', pix565))
# RLE8: each row: 4 pixels as encoded runs
rle = b''
for y in range(H - 1, -1, -1):
    for x in range(W): rle += bytes([1, (x + y) % 4])
    rle += b'\0\0'
rle = rle[:-2] + b'\0\1'
pal4 = b''.join(bytes([c[2], c[1], c[0], 0]) for c in cols)
open(f'{out}/rle8.bmp', 'wb').write(bmp_file(struct.pack('<IiiHHIIiiII', 40, W, H, 1, 8, 1, len(rle), 3780, 3780, 4, 0), pal4, rle))
# EXIF with sub-IFDs and a thumbnail
def tiff_ifd(entries, base, next_off=0):
    # entries: (tag, type, count, valuebytes); returns bytes of IFD placed at base, with out-of-line data after
    n = len(entries); head = struct.pack('<H', n); data = b''; body = b''
    data_off = base + 2 + 12 * n + 4
    for tag, typ, cnt, val in entries:
        if len(val) <= 4: body += struct.pack('<HHI', tag, typ, cnt) + val.ljust(4, b'\0')
        else:
            body += struct.pack('<HHII', tag, typ, cnt, data_off + len(data)); data += val
            if len(data) % 2: data += b'\0'
    return head + body + struct.pack('<I', next_off) + data
thumb = io.BytesIO(); rgb.resize((2, 2)).save(thumb, 'JPEG', quality=80); thumb = thumb.getvalue()
def build_exif():
    # layout: header(8) | IFD0 | ExifIFD | GPS | IFD1 | thumb
    def s(v): return v.encode() + b'\0'
    r = lambda a, b: struct.pack('<II', a, b)
    exif_entries = [(0x829A, 5, 1, r(1, 125)), (0x8827, 3, 1, struct.pack('<H', 200)), (0x9000, 7, 4, b'0230'),
                    (0x9003, 2, 20, s('2026:10:27 10:11:12')), (0x9286, 7, 12, b'ASCII\0\0\0hey!'), (0x9204, 10, 1, struct.pack('<ii', -1, 3))]
    gps_entries = [(0x0000, 1, 4, bytes([2, 3, 0, 0])), (0x0001, 2, 2, b'N\0'), (0x0002, 5, 3, r(10, 1) + r(20, 1) + r(30, 1))]
    for _ in range(3):
        pass
    # iterate to fix offsets
    ifd0_base = 8
    def ifd0(exif_off, gps_off, ifd1_off):
        return tiff_ifd([(0x010F, 2, 6, s('Maker')), (0x0112, 3, 1, struct.pack('<H', 6)), (0x011A, 5, 1, r(300, 1)), (0x011B, 5, 1, r(300, 1)),
                         (0x0128, 3, 1, struct.pack('<H', 2)), (0x8769, 4, 1, struct.pack('<I', exif_off)), (0x8825, 4, 1, struct.pack('<I', gps_off))], ifd0_base, ifd1_off)
    a = ifd0(0, 0, 0); exif_off = ifd0_base + len(a)
    e = tiff_ifd(exif_entries, exif_off); gps_off = exif_off + len(e)
    g = tiff_ifd(gps_entries, gps_off); ifd1_off = gps_off + len(g)
    def ifd1(toff): return tiff_ifd([(0x0103, 3, 1, struct.pack('<H', 6)), (0x011A, 5, 1, r(72, 1)), (0x011B, 5, 1, r(72, 1)), (0x0128, 3, 1, struct.pack('<H', 2)),
                                     (0x0201, 4, 1, struct.pack('<I', toff)), (0x0202, 4, 1, struct.pack('<I', len(thumb)))], ifd1_off)
    t1 = ifd1(0); toff = ifd1_off + len(t1)
    return b'II*\0' + struct.pack('<I', 8) + ifd0(exif_off, gps_off, ifd1_off) + e + g + ifd1(toff) + thumb
rgb.save(f'{out}/exif2.jpg', quality=90, exif=b'Exif\0\0' + build_exif())
# GIF with a partial second frame, disposal and delay
def gif_partial():
    lzw_min = 2
    def lzw(indices):
        # trivial: clear code before every pixel (valid LZW with code size 3)
        clear = 1 << lzw_min; eoi = clear + 1; bits = []; size = lzw_min + 1
        codes = []
        for i in indices: codes += [clear, i]
        codes.append(eoi)
        acc = 0; n = 0; outb = b''
        for c in codes:
            acc |= c << n; n += size
            while n >= 8: outb += bytes([acc & 255]); acc >>= 8; n -= 8
        if n: outb += bytes([acc & 255])
        return bytes([lzw_min]) + b''.join(bytes([len(outb[i:i + 255])]) + outb[i:i + 255] for i in range(0, len(outb), 255)) + b'\0'
    b = b'GIF89a' + struct.pack('<HHBBB', W, H, 0xF1, 0, 0) + pal
    b += b'\x21\xff\x0bNETSCAPE2.0\x03\x01' + struct.pack('<H', 0) + b'\0'
    b += b'\x21\xf9\x04' + bytes([0x04]) + struct.pack('<H', 7) + b'\0\0'  # disposal 1 (leave), delay 7
    b += b'\x2c' + struct.pack('<HHHHB', 0, 0, W, H, 0) + lzw([(x + y) % 4 for y in range(H) for x in range(W)])
    b += b'\x21\xf9\x04' + bytes([0x09]) + struct.pack('<H', 9) + b'\x03\0'  # disposal 2, transparent idx 3, delay 9
    b += b'\x2c' + struct.pack('<HHHHB', 1, 1, 2, 2, 0) + lzw([2, 3, 3, 2])
    b += b'\x21\xfe' + bytes([4]) + b'note' + b'\0'
    return b + b'\x3b'
open(f'{out}/partial.gif', 'wb').write(gif_partial())
