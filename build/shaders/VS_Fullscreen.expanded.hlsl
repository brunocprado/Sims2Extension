// @shader VS_Fullscreen VSMain vs_3_0
// Full-screen triangle-pair vertex shader (vertices already in clip space).
// c0.xy = 1 / render target size, used for the D3D9 half pixel offset.
float4 c_texel : register(c0);

struct VSOut {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(float4 p : POSITION, float2 uv : TEXCOORD0) {
    VSOut o;
    o.pos = float4(p.x - c_texel.x, p.y + c_texel.y, 0.0, 1.0);
    o.uv  = uv;
    return o;
}
