// @shader PS_Blur PSMain ps_3_0
// Depth aware (bilateral) separable blur of the AO / shadow buffer.
// Input: R = ao, G = shadow, B = view depth.   c1.xy = blur direction in texels (1,0) or (0,1).

sampler2D sAO : register(s0);   // point sampled

float4 c_tex : register(c0);    // 1/W, 1/H, W, H
float4 c_dir : register(c1);    // direction (texel units), depth tolerance (relative), unused

float4 PSMain(float2 uv : TEXCOORD0) : COLOR0 {
    float4 c = tex2Dlod(sAO, float4(uv, 0, 0));
    if (c.b > 500.0) return c;      // sky

    float2 step = c_dir.xy * c_tex.xy;
    float tol = max(c_dir.z, 0.001);
    float2 sum = c.rg;
    float  wsum = 1.0;
    [unroll] for (int i = 1; i <= 6; ++i) {
        float g = exp(-0.5 * i * i / 9.0);
        [unroll] for (int s = -1; s <= 1; s += 2) {
            float2 suv = uv + step * (i * s);
            float4 t = tex2Dlod(sAO, float4(suv, 0, 0));
            float dz = abs(t.b - c.b) / max(c.b, 0.1);
            float w = g * saturate(1.0 - dz / tol) * (t.b < 500.0 ? 1.0 : 0.0);
            sum  += t.rg * w;
            wsum += w;
        }
    }
    return float4(sum / wsum, c.b, 1.0);
}
