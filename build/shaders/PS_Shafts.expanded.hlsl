// @shader PS_Shafts PSMain ps_3_0
// Screen-space light shafts (god rays): radial blur of the bright parts of the image (and of any visible sky)
// towards the sun's position on screen. Runs at 1/4 resolution; the result is added in the final pass.

sampler2D sColor : register(s0);   // image (bilinear)
sampler2D sDepth : register(s1);   // INTZ depth (point)

float4 c_tex : register(c0);   // 1/BW, 1/BH, BW, BH
float4 c_sun : register(c1);   // sun position (uv), strength, brightness threshold
float4 c_col : register(c2);   // sun colour rgb, per-sample decay

float IGN(float2 pix) { return frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715)))); }

float4 PSMain(float2 uv : TEXCOORD0) : COLOR0 {
    const int N = 36;
    float2 stepv = (c_sun.xy - uv) * (0.75 / N);
    float2 p = uv + stepv * IGN(uv * c_tex.zw);
    float acc = 0.0;
    float w = 1.0;
    [loop] for (int i = 0; i < N; ++i) {
        float3 col = tex2Dlod(sColor, float4(p, 0, 0)).rgb;
        float lum = dot(col, float3(0.299, 0.587, 0.114));
        float m = saturate((lum - c_sun.w) / max(1.0 - c_sun.w, 0.05));
        float d = tex2Dlod(sDepth, float4(p, 0, 0)).r;
        if (d >= 0.99999) m = max(m, 0.6);       // open sky emits
        if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) m = 0.0;
        acc += m * w;
        w *= c_col.w;
        p += stepv;
    }
    acc = acc / N * c_sun.z;
    return float4(c_col.rgb * acc, 1.0);
}
