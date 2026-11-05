#pragma once
#include "ts2/win32_abi.hpp"

// ============================================================================
// Direct3D 9 ABI extensions used by the post-processing / lighting module.
// Everything is accessed through raw vtable slots so no DirectX SDK is required.
// ============================================================================
namespace d3d9 {

// ---- IDirect3DDevice9 vtable slots --------------------------------------------------------------------------
namespace slot {
    constexpr int Release                 = 2;
    constexpr int GetDirect3D             = 6;
    constexpr int GetDeviceCaps           = 7;
    constexpr int Reset                   = 16;
    constexpr int Present                 = 17;
    constexpr int GetBackBuffer           = 18;
    constexpr int CreateTexture           = 23;
    constexpr int CreateRenderTarget      = 28;
    constexpr int CreateDepthStencilSurface = 29;
    constexpr int StretchRect             = 34;
    constexpr int SetRenderTarget         = 37;
    constexpr int GetRenderTarget         = 38;
    constexpr int SetDepthStencilSurface  = 39;
    constexpr int GetDepthStencilSurface  = 40;
    constexpr int BeginScene              = 41;
    constexpr int EndScene                = 42;
    constexpr int Clear                   = 43;
    constexpr int SetTransform            = 44;
    constexpr int GetTransform            = 45;
    constexpr int SetViewport             = 47;
    constexpr int GetViewport             = 48;
    constexpr int SetLight                = 51;
    constexpr int LightEnable             = 53;
    constexpr int SetRenderState          = 57;
    constexpr int GetRenderState          = 58;
    constexpr int CreateStateBlock        = 59;
    constexpr int SetTexture              = 65;
    constexpr int SetTextureStageState    = 67;
    constexpr int SetSamplerState         = 69;
    constexpr int SetScissorRect          = 75;
    constexpr int DrawPrimitive           = 81;
    constexpr int DrawIndexedPrimitive    = 82;
    constexpr int DrawPrimitiveUP         = 83;
    constexpr int DrawIndexedPrimitiveUP  = 84;
    constexpr int CreateVertexDeclaration = 86;
    constexpr int SetVertexDeclaration    = 87;
    constexpr int SetFVF                  = 89;
    constexpr int CreateVertexShader      = 91;
    constexpr int SetVertexShader         = 92;
    constexpr int SetVertexShaderConstantF= 94;
    constexpr int SetStreamSource         = 100;
    constexpr int SetIndices              = 104;
    constexpr int CreatePixelShader       = 106;
    constexpr int SetPixelShader          = 107;
    constexpr int SetPixelShaderConstantF = 109;
}

// IDirect3D9 vtable slots
namespace d3dslot {
    constexpr int CheckDeviceFormat = 10;
}

// ---- Enums / constants --------------------------------------------------------------------------------------
constexpr DWORD FMT_UNKNOWN        = 0;
constexpr DWORD FMT_A8R8G8B8       = 21;
constexpr DWORD FMT_X8R8G8B8       = 22;
constexpr DWORD FMT_A2R10G10B10    = 35;
constexpr DWORD FMT_L8             = 50;
constexpr DWORD FMT_D24S8          = 75;
constexpr DWORD FMT_D24X8          = 77;
constexpr DWORD FMT_D16            = 80;
constexpr DWORD FMT_G16R16F        = 112;
constexpr DWORD FMT_A16B16G16R16F  = 113;
constexpr DWORD FMT_R32F           = 114;
constexpr DWORD FMT_A32B32G32R32F  = 116;
constexpr DWORD FMT_R16F           = 111;
constexpr DWORD FMT_INTZ           = 0x5A544E49u; // MAKEFOURCC('I','N','T','Z')
constexpr DWORD FMT_DF24           = 0x34324644u; // 'DF24'

constexpr DWORD USAGE_RENDERTARGET = 0x00000001;
constexpr DWORD USAGE_DEPTHSTENCIL = 0x00000002;
constexpr DWORD POOL_DEFAULT       = 0;
constexpr DWORD RTYPE_TEXTURE      = 3;

constexpr DWORD CLEAR_TARGET  = 0x1;
constexpr DWORD CLEAR_ZBUFFER = 0x2;
constexpr DWORD CLEAR_STENCIL = 0x4;

// D3DTRANSFORMSTATETYPE
constexpr DWORD TS_VIEW       = 2;
constexpr DWORD TS_PROJECTION = 3;
constexpr DWORD TS_WORLD      = 256;

// More render states
constexpr DWORD RS_ZFUNC             = 23;
constexpr DWORD RS_ALPHAREF          = 24;
constexpr DWORD RS_ALPHAFUNC         = 25;
constexpr DWORD RS_STENCILFUNC       = 56;
constexpr DWORD RS_AMBIENT           = 139;
constexpr DWORD RS_SRGBWRITEENABLE   = 194;
constexpr DWORD RS_COLORWRITEENABLE  = 168;
constexpr DWORD RS_SEPARATEALPHABLEND = 206;

// Sampler states
constexpr DWORD SAMP_ADDRESSU  = 1;
constexpr DWORD SAMP_ADDRESSV  = 2;
constexpr DWORD SAMP_MAGFILTER = 5;
constexpr DWORD SAMP_MINFILTER = 6;
constexpr DWORD SAMP_MIPFILTER = 7;
constexpr DWORD SAMP_SRGBTEXTURE = 11;
constexpr DWORD TADDRESS_CLAMP = 3;
constexpr DWORD TEXF_NONE   = 0;
constexpr DWORD TEXF_POINT  = 1;
constexpr DWORD TEXF_LINEAR = 2;

struct D3DSURFACE_DESC {
    DWORD Format;
    DWORD Type;
    DWORD Usage;
    DWORD Pool;
    DWORD MultiSampleType;
    DWORD MultiSampleQuality;
    UINT  Width;
    UINT  Height;
};

struct D3DMATRIX {
    float m[4][4];
};

struct D3DVECTOR { float x, y, z; };
struct D3DCOLORVALUE { float r, g, b, a; };

struct D3DLIGHT9 {
    DWORD         Type;
    D3DCOLORVALUE Diffuse;
    D3DCOLORVALUE Specular;
    D3DCOLORVALUE Ambient;
    D3DVECTOR     Position;
    D3DVECTOR     Direction;
    float         Range;
    float         Falloff;
    float         Attenuation0;
    float         Attenuation1;
    float         Attenuation2;
    float         Theta;
    float         Phi;
};

// Vertex element declaration (IDirect3DVertexDeclaration9)
struct D3DVERTEXELEMENT9 {
    WORD Stream;
    WORD Offset;
    BYTE Type;
    BYTE Method;
    BYTE Usage;
    BYTE UsageIndex;
};

constexpr BYTE DECLTYPE_FLOAT2 = 1;
constexpr BYTE DECLTYPE_FLOAT3 = 2;
constexpr BYTE DECLTYPE_FLOAT4 = 3;
constexpr BYTE DECLUSAGE_POSITION = 0;
constexpr BYTE DECLUSAGE_TEXCOORD = 5;

struct FullscreenVertex {
    float x, y, z, w;
    float u, v;
};

// ---- Tiny COM helpers (raw vtable calls) ----------------------------------------------------------------------
struct IUnknownLike { void** vtable; };

inline ULONG ComRelease(void* obj) {
    if (!obj) return 0;
    auto* u = static_cast<IUnknownLike*>(obj);
    return reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>(u->vtable[2])(obj);
}
inline ULONG ComAddRef(void* obj) {
    if (!obj) return 0;
    auto* u = static_cast<IUnknownLike*>(obj);
    return reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>(u->vtable[1])(obj);
}

// IDirect3DSurface9::GetDesc (slot 12)
inline HRESULT SurfaceGetDesc(void* surf, D3DSURFACE_DESC* desc) {
    auto* u = static_cast<IUnknownLike*>(surf);
    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, D3DSURFACE_DESC*)>(u->vtable[12])(surf, desc);
}
// IDirect3DTexture9::GetSurfaceLevel (slot 18)
inline HRESULT TextureGetSurfaceLevel(void* tex, UINT level, void** outSurface) {
    auto* u = static_cast<IUnknownLike*>(tex);
    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, UINT, void**)>(u->vtable[18])(tex, level, outSurface);
}

} // namespace d3d9
