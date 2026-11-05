// @shader PS_BloomBright PSBright ps_3_0
// @shader PS_BloomBlur PSBlur ps_3_0
// Bloom: bright pass (down sampled 4x with a 4x4 box) and separable gaussian blur on the small targets.

sampler2D sImg : register(s0);

float4 c_tex   : register(c0);   // 1/W, 1/H of the *source* texture
float4 c_bloom : register(c1);   // threshold, softness, 0, 0
float4 c_dir   : register(c2);   // blur direction in texels (1,0)/(0,1)

float4 PSBright(float2 uv : TEXCOORD0) : COLOR0 {
    float3 sum = 0;
    [unroll] for (int y = -1; y <= 2; ++y)
        [unroll] for (int x = -1; x <= 2; ++x) {
            float3 c = tex2Dlod(sImg, float4(uv + (float2(x, y) - 0.5) * c_tex.xy, 0, 0)).rgb;
            float l = max(c.r, max(c.g, c.b));
            float k = saturate((l - c_bloom.x) / max(c_bloom.y, 0.001));
            sum += c * k;
        }
    return float4(sum / 16.0, 1);
}

float4 PSBlur(float2 uv : TEXCOORD0) : COLOR0 {
    static const float w[5] = { 0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162 };
    float3 acc = tex2Dlod(sImg, float4(uv, 0, 0)).rgb * w[0];
    float2 step = c_dir.xy * c_tex.xy;
    [unroll] for (int i = 1; i < 5; ++i) {
        acc += tex2Dlod(sImg, float4(uv + step * i, 0, 0)).rgb * w[i];
        acc += tex2Dlod(sImg, float4(uv - step * i, 0, 0)).rgb * w[i];
    }
    return float4(acc, 1);
}
