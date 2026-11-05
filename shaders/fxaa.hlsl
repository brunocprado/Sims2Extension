// @shader PS_FXAA PSMain ps_3_0
// FXAA (Timothy Lottes, public domain "FXAA 2" style edge search, tuned a little for UI-free scene images).

sampler2D sImg : register(s0);   // bilinear filtered

float4 c_tex : register(c0);     // 1/W, 1/H, W, H

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 PSMain(float2 uv : TEXCOORD0) : COLOR0 {
    float2 rcp = c_tex.xy;
    float3 rgbNW = tex2Dlod(sImg, float4(uv + float2(-1, -1) * rcp, 0, 0)).rgb;
    float3 rgbNE = tex2Dlod(sImg, float4(uv + float2( 1, -1) * rcp, 0, 0)).rgb;
    float3 rgbSW = tex2Dlod(sImg, float4(uv + float2(-1,  1) * rcp, 0, 0)).rgb;
    float3 rgbSE = tex2Dlod(sImg, float4(uv + float2( 1,  1) * rcp, 0, 0)).rgb;
    float4 m = tex2Dlod(sImg, float4(uv, 0, 0));
    float3 rgbM = m.rgb;

    float lNW = Luma(rgbNW), lNE = Luma(rgbNE), lSW = Luma(rgbSW), lSE = Luma(rgbSE), lM = Luma(rgbM);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    // early out on flat regions
    if (lMax - lMin < max(0.0312, lMax * 0.125)) return m;

    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));
    float dirReduce = max((lNW + lNE + lSW + lSE) * (0.25 * (1.0 / 8.0)), 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -8.0, 8.0) * rcp;

    float3 rgbA = 0.5 * (tex2Dlod(sImg, float4(uv + dir * (1.0 / 3.0 - 0.5), 0, 0)).rgb +
                         tex2Dlod(sImg, float4(uv + dir * (2.0 / 3.0 - 0.5), 0, 0)).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (tex2Dlod(sImg, float4(uv + dir * -0.5, 0, 0)).rgb +
                                       tex2Dlod(sImg, float4(uv + dir *  0.5, 0, 0)).rgb);
    float lB = Luma(rgbB);
    return float4((lB < lMin || lB > lMax) ? rgbA : rgbB, m.a);
}
