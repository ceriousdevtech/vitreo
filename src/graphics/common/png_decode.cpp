// SPDX-License-Identifier: LGPL-2.1-only
#include "graphics/common/png_decode.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <zlib.h>
namespace vitreo::graphics::png_decode {
bool inflate_all(std::span<const u8> in, std::vector<u8>& out, usize expect) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return false;
    out.resize(expect ? expect : in.size() * 4 + 64);
    z.next_in = const_cast<Bytef*>(in.data());
    z.avail_in = static_cast<uInt>(in.size());
    int r = Z_OK;
    while (r == Z_OK) {
        if (z.total_out >= out.size()) out.resize(out.size() * 2 + 64);
        z.next_out = out.data() + z.total_out;
        z.avail_out = static_cast<uInt>(out.size() - z.total_out);
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_BUF_ERROR && z.avail_in == 0) break;
        if (r == Z_BUF_ERROR) r = Z_OK;
    }
    out.resize(z.total_out);
    inflateEnd(&z);
    return r == Z_STREAM_END || (expect && out.size() >= expect);
}

u8 paeth(u8 a, u8 b, u8 c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

bool unfilter(u8* data, usize rows, usize row_bytes, usize bpp, std::span<const u8> src, usize& at) {
    std::vector<u8> prev(row_bytes, 0);
    for (usize y = 0; y < rows; ++y) {
        if (at + 1 + row_bytes > src.size()) return false;
        const u8 ft = src[at++];
        u8* row = data + y * row_bytes;
        std::memcpy(row, src.data() + at, row_bytes);
        at += row_bytes;
        for (usize i = 0; i < row_bytes; ++i) {
            const u8 a = i >= bpp ? row[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
            switch (ft) {
                case 0: break;
                case 1: row[i] = static_cast<u8>(row[i] + a); break;
                case 2: row[i] = static_cast<u8>(row[i] + b); break;
                case 3: row[i] = static_cast<u8>(row[i] + ((a + b) >> 1)); break;
                case 4: row[i] = static_cast<u8>(row[i] + paeth(a, b, c)); break;
                default: return false;
            }
        }
        std::memcpy(prev.data(), row, row_bytes);
    }
    return true;
}


bool samples(const Header& info,std::span<const u8> compressed,Pixels& out){
 const u32 w=info.width,h=info.height;const u8 depth=info.depth,color=info.color;
 const u32 channels=color==0?1:color==2?3:color==3?1:color==4?2:color==6?4:0;
 if(!w||!h||!channels||info.interlace>1)return false;
 if(depth!=1&&depth!=2&&depth!=4&&depth!=8&&depth!=16)return false;
 if((color==2||color==4||color==6)&&depth<8)return false;
 if(color==3&&depth==16)return false;
 const usize bits=channels*depth,bpp=std::max<usize>(1,(bits+7)/8);
 const u64 stride=(u64(w)*bits+7)/8;
 if(stride>std::numeric_limits<usize>::max()/h)return false;
 std::vector<u8> raw;if(!inflate_all(compressed,raw,0))return false;
 out.stride=static_cast<usize>(stride);out.rows.assign(out.stride*h,0);usize at=0;
 if(!info.interlace)return unfilter(out.rows.data(),h,out.stride,bpp,raw,at);
 // PNG Adam7 pass geometry, W3C PNG specification section8.
 constexpr u32 sx[]={0,4,0,2,0,1,0},sy[]={0,0,4,0,2,0,1};
 constexpr u32 dx[]={8,8,4,4,2,2,1},dy[]={8,8,8,4,4,2,2};
 for(usize pass=0;pass<7;++pass){
  const usize pw=w>sx[pass]?(w-sx[pass]+dx[pass]-1)/dx[pass]:0,ph=h>sy[pass]?(h-sy[pass]+dy[pass]-1)/dy[pass]:0;
  if(!pw||!ph)continue;const usize row_bytes=(pw*bits+7)/8;std::vector<u8> rows(row_bytes*ph);
  if(!unfilter(rows.data(),ph,row_bytes,bpp,raw,at))return false;
  for(usize y=0;y<ph;++y)for(usize x=0;x<pw;++x){const usize tx=sx[pass]+x*dx[pass],ty=sy[pass]+y*dy[pass];const auto* source=rows.data()+y*row_bytes;auto* target=out.rows.data()+ty*out.stride;
   if(bits>=8){std::memcpy(target+tx*(bits/8),source+x*(bits/8),bits/8);continue;}
   const auto value=(source[x*bits/8]>>(8-bits-(x*bits%8)))&((1u<<bits)-1);target[tx*bits/8]|=static_cast<u8>(value<<(8-bits-(tx*bits%8)));
  }
 }
 return true;
}
}
