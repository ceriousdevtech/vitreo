// Original generic immutable-global and mutable-local regression fixtures.
static const uint A[4] = {0x7fa12345u,0x80000000u,0xffffffffu,0x12345678u};
static const uint B[4] = {0xdeadbeefu,0x7fc54321u,0x80000001u,42u};
uint4 ps32(nointerpolation uint4 index : INDEX0) : SV_Target0 {
    uint i=index.x&3, j=index.y&3;
    uint local[4]={11,22,33,44};
    local[i]=index.z;
    return uint4(A[i],B[j],local[i],local[j]);
}
static const uint16_t U16[4] = {uint16_t(0),uint16_t(65535),uint16_t(32768),uint16_t(4660)};
static const float16_t F16[4] = {float16_t(0),float16_t(-0.0),float16_t(1.5),float16_t(-2.0)};
uint4 ps16(nointerpolation uint4 index : INDEX0) : SV_Target0 {
    uint i=index.x&3, j=index.y&3;
    return uint4(uint(U16[i]),asuint(float(F16[j])),uint(U16[j]),asuint(float(F16[i])));
}
static const uint64_t U64[4] = {0x0123456789abcdefull,0xffffffffffffffffull,0x8000000000000000ull,42ull};
static const double D64[4] = {0.0,-0.0,1.5,-2.25};
uint4 ps64(nointerpolation uint4 index : INDEX0) : SV_Target0 {
    uint i=index.x&3, j=index.y&3;
    uint64_t u=U64[i]; uint lo,hi; asuint(D64[j],lo,hi);
    return uint4(uint(u),uint(u>>32),lo,hi);
}
static const uint2 N[2][3] = {{uint2(1,11),uint2(2,22),uint2(3,33)}, {uint2(4,44),uint2(5,55),uint2(6,66)}};
uint4 psnested(nointerpolation uint4 index : INDEX0) : SV_Target0 {
    uint2 v=N[index.x&1][index.y%3]; return uint4(v,N[1][index.z%3]);
}
