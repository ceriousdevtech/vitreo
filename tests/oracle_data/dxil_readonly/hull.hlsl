// Same readonly global is used by both control-point and patch functions.
static const uint Shared[4]={3,5,7,11};
static const uint PatchOnly[4]={13,17,19,23};
struct CP { float4 p:SV_Position; uint i:INDEX; };
struct PC { float e[3]:SV_TessFactor; float inside:SV_InsideTessFactor; uint value:VALUE; };
PC patch(InputPatch<CP,3> cp,uint pid:SV_PrimitiveID) {
    PC o; o.e[0]=o.e[1]=o.e[2]=1; o.inside=1;
    o.value=Shared[cp[0].i&3]+PatchOnly[pid&3]; return o;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("patch")]
CP hs(InputPatch<CP,3> cp,uint id:SV_OutputControlPointID) {
    CP o=cp[id]; o.i=Shared[o.i&3]; return o;
}
