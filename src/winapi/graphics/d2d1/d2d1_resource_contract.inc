#pragma once
#include <vitreo/common.hpp>
#include <array>
// Generated SDK inherited COM method inventory; MSVC aggregate result ABI
// recorded in tools/oracle_probe/d2d1_return_abi.cpp + .asm oracle artifacts.
namespace vitreo::winapi::d2d1_contract {
struct ResourceMethod {const char* name;u16 bytes;u8 arguments;const char* result;bool aggregate;};
inline constexpr ResourceMethod target_methods[]={
{"QueryInterface",12,2,"HRESULT",false},
{"AddRef",4,0,"ULONG",false},
{"Release",4,0,"ULONG",false},
{"GetFactory",8,1,"void",false},
{"CreateBitmap",28,5,"HRESULT",false},
{"CreateBitmapFromWicBitmap",16,3,"HRESULT",false},
{"CreateSharedBitmap",20,4,"HRESULT",false},
{"CreateBitmapBrush",20,4,"HRESULT",false},
{"CreateSolidColorBrush",16,3,"HRESULT",false},
{"CreateGradientStopCollection",24,5,"HRESULT",false},
{"CreateLinearGradientBrush",20,4,"HRESULT",false},
{"CreateRadialGradientBrush",20,4,"HRESULT",false},
{"CreateCompatibleRenderTarget",24,5,"HRESULT",false},
{"CreateLayer",12,2,"HRESULT",false},
{"CreateMesh",8,1,"HRESULT",false},
{"DrawLine",32,5,"void",false},
{"DrawRectangle",20,4,"void",false},
{"FillRectangle",12,2,"void",false},
{"DrawRoundedRectangle",20,4,"void",false},
{"FillRoundedRectangle",12,2,"void",false},
{"DrawEllipse",20,4,"void",false},
{"FillEllipse",12,2,"void",false},
{"DrawGeometry",20,4,"void",false},
{"FillGeometry",16,3,"void",false},
{"FillMesh",12,2,"void",false},
{"FillOpacityMask",24,5,"void",false},
{"DrawBitmap",24,5,"void",false},
{"DrawText",32,7,"void",false},
{"DrawTextLayout",24,4,"void",false},
{"DrawGlyphRun",24,4,"void",false},
{"SetTransform",8,1,"void",false},
{"GetTransform",8,1,"void",false},
{"SetAntialiasMode",8,1,"void",false},
{"GetAntialiasMode",4,0,"D2D1_ANTIALIAS_MODE",false},
{"SetTextAntialiasMode",8,1,"void",false},
{"GetTextAntialiasMode",4,0,"D2D1_TEXT_ANTIALIAS_MODE",false},
{"SetTextRenderingParams",8,1,"void",false},
{"GetTextRenderingParams",8,1,"void",false},
{"SetTags",20,2,"void",false},
{"GetTags",12,2,"void",false},
{"PushLayer",12,2,"void",false},
{"PopLayer",4,0,"void",false},
{"Flush",12,2,"HRESULT",false},
{"SaveDrawingState",8,1,"void",false},
{"RestoreDrawingState",8,1,"void",false},
{"PushAxisAlignedClip",12,2,"void",false},
{"PopAxisAlignedClip",4,0,"void",false},
{"Clear",8,1,"void",false},
{"BeginDraw",4,0,"void",false},
{"EndDraw",12,2,"HRESULT",false},
{"GetPixelFormat",8,1,"D2D1_PIXEL_FORMAT",true},
{"SetDpi",12,2,"void",false},
{"GetDpi",12,2,"void",false},
{"GetSize",8,1,"D2D1_SIZE_F",true},
{"GetPixelSize",8,1,"D2D1_SIZE_U",true},
{"GetMaximumBitmapSize",4,0,"UINT32",false},
{"IsSupported",8,1,"BOOL",false},
{"BindDC",12,2,"HRESULT",false},
};
enum TargetSlot { target_QueryInterface=0,target_AddRef=1,target_Release=2,target_GetFactory=3,target_CreateBitmap=4,target_CreateBitmapFromWicBitmap=5,target_CreateSharedBitmap=6,target_CreateBitmapBrush=7,target_CreateSolidColorBrush=8,target_CreateGradientStopCollection=9,target_CreateLinearGradientBrush=10,target_CreateRadialGradientBrush=11,target_CreateCompatibleRenderTarget=12,target_CreateLayer=13,target_CreateMesh=14,target_DrawLine=15,target_DrawRectangle=16,target_FillRectangle=17,target_DrawRoundedRectangle=18,target_FillRoundedRectangle=19,target_DrawEllipse=20,target_FillEllipse=21,target_DrawGeometry=22,target_FillGeometry=23,target_FillMesh=24,target_FillOpacityMask=25,target_DrawBitmap=26,target_DrawText=27,target_DrawTextLayout=28,target_DrawGlyphRun=29,target_SetTransform=30,target_GetTransform=31,target_SetAntialiasMode=32,target_GetAntialiasMode=33,target_SetTextAntialiasMode=34,target_GetTextAntialiasMode=35,target_SetTextRenderingParams=36,target_GetTextRenderingParams=37,target_SetTags=38,target_GetTags=39,target_PushLayer=40,target_PopLayer=41,target_Flush=42,target_SaveDrawingState=43,target_RestoreDrawingState=44,target_PushAxisAlignedClip=45,target_PopAxisAlignedClip=46,target_Clear=47,target_BeginDraw=48,target_EndDraw=49,target_GetPixelFormat=50,target_SetDpi=51,target_GetDpi=52,target_GetSize=53,target_GetPixelSize=54,target_GetMaximumBitmapSize=55,target_IsSupported=56,target_BindDC=57 };
inline constexpr ResourceMethod brush_methods[]={
{"QueryInterface",12,2,"HRESULT",false},
{"AddRef",4,0,"ULONG",false},
{"Release",4,0,"ULONG",false},
{"GetFactory",8,1,"void",false},
{"SetOpacity",8,1,"void",false},
{"SetTransform",8,1,"void",false},
{"GetOpacity",4,0,"FLOAT",false},
{"GetTransform",8,1,"void",false},
{"SetColor",8,1,"void",false},
{"GetColor",8,1,"D2D1_COLOR_F",true},
};
enum BrushSlot { brush_QueryInterface=0,brush_AddRef=1,brush_Release=2,brush_GetFactory=3,brush_SetOpacity=4,brush_SetTransform=5,brush_GetOpacity=6,brush_GetTransform=7,brush_SetColor=8,brush_GetColor=9 };
inline constexpr std::array<u8,16> iid_ID2D1Resource={0x91,0x6,0xd9,0x2c,0xe2,0x12,0xdc,0x11,0x9f,0xed,0x0,0x11,0x43,0xa0,0x55,0xf9};
inline constexpr std::array<u8,16> iid_ID2D1Brush={0xa8,0x6,0xd9,0x2c,0xe2,0x12,0xdc,0x11,0x9f,0xed,0x0,0x11,0x43,0xa0,0x55,0xf9};
inline constexpr std::array<u8,16> iid_ID2D1SolidColorBrush={0xa9,0x6,0xd9,0x2c,0xe2,0x12,0xdc,0x11,0x9f,0xed,0x0,0x11,0x43,0xa0,0x55,0xf9};
inline constexpr std::array<u8,16> iid_ID2D1RenderTarget={0x94,0x6,0xd9,0x2c,0xe2,0x12,0xdc,0x11,0x9f,0xed,0x0,0x11,0x43,0xa0,0x55,0xf9};
inline constexpr std::array<u8,16> iid_ID2D1DCRenderTarget={0x64,0xbc,0x51,0x1c,0x61,0xde,0xfd,0x46,0x98,0x99,0x63,0xa5,0xd8,0xf0,0x39,0x50};
inline constexpr u32 D2DERR_WRONG_STATE=0x88990001u;
inline constexpr u32 D2DERR_WRONG_FACTORY=0x88990012u;
inline constexpr u32 D2DERR_WRONG_RESOURCE_DOMAIN=0x88990015u;
inline constexpr u32 D2DERR_POP_CALL_DID_NOT_MATCH_PUSH=0x88990014u;
inline constexpr u32 D2DERR_PUSH_POP_UNBALANCED=0x88990016u;
inline constexpr u32 WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT=0x88982F80u;
inline constexpr usize layout_props=28;
inline constexpr usize layout_type=0;
inline constexpr usize layout_format=4;
inline constexpr usize layout_dpiX=12;
inline constexpr usize layout_dpiY=16;
inline constexpr usize layout_usage=20;
inline constexpr usize layout_level=24;
inline constexpr usize layout_brush=28;
inline constexpr usize layout_color=16;
inline constexpr u32 D3D_FEATURE_LEVEL_9_1=0x9100;
inline constexpr u32 D3D_FEATURE_LEVEL_10_0=0xa000;
inline constexpr u32 D2D1_ALPHA_MODE_PREMULTIPLIED=1;
inline constexpr u32 D2D1_ALPHA_MODE_IGNORE=3;
inline constexpr u32 DXGI_FORMAT_B8G8R8A8_UNORM=87;
inline constexpr u32 D2D1_RENDER_TARGET_TYPE_DEFAULT=0;
inline constexpr u32 D2D1_RENDER_TARGET_TYPE_SOFTWARE=1;
inline constexpr u32 D2D1_RENDER_TARGET_TYPE_HARDWARE=2;
inline constexpr u32 D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE=0x00000002;
inline constexpr u32 D2D1_RENDER_TARGET_USAGE_FORCE_BITMAP_REMOTING=0x00000001;
inline constexpr u32 D2D1_ANTIALIAS_MODE_PER_PRIMITIVE=0;
inline constexpr u32 D2D1_ANTIALIAS_MODE_ALIASED=1;
inline constexpr float dip_screen_dpi=96.f;
}
