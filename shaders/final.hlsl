// @shader PS_Final PSMain ps_3_0
// Final grade: bloom + light shafts, exposure, filmic tone mapping, time-of-day tint, contrast, saturation, vignette.

sampler2D sImg   : register(s0);   // anti-aliased scene
sampler2D sBloom : register(s1);   // blurred bright pass (bilinear)
sampler2D sShaft : register(s2);   // light shafts (bilinear, already tinted)

float4 c_tex   : register(c0);
float4 c_grade : register(c1);   // bloomStrength, vignette, saturation, contrast
float4 c_misc  : register(c2);   // exposure, tonemapStrength, tonemapExposure, shaftsOn
float4 c_tint  : register(c3);   // time-of-day tint rgb (1 = neutral), 0

// Narkowicz ACES fit (linear in, linear out)
float3 ACES(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float4 PSMain(float2 uv : TEXCOORD0) : COLOR0 {
    float4 s = tex2D(sImg, uv);
    float3 c = s.rgb;
    c += tex2D(sBloom, uv).rgb * c_grade.x;
    c += tex2D(sShaft, uv).rgb * c_misc.w;
    c *= c_misc.x;

    // filmic curve: applied in linear light, exposure chosen so mid grey stays put and the S-curve adds depth
    if (c_misc.y > 0.001) {
        float3 lin = pow(saturate(c), 2.2);
        float k = c_misc.z;
        float3 tm = ACES(lin * k) / ACES(float3(k, k, k)).x;
        c = lerp(c, pow(saturate(tm), 1.0 / 2.2), c_misc.y);
    }
    c *= c_tint.rgb;

    // contrast around mid grey, then saturation
    c = (c - 0.5) * c_grade.w + 0.5;
    float l = dot(c, float3(0.299, 0.587, 0.114));
    c = lerp(l.xxx, c, c_grade.z);
    // vignette
    float2 q = uv - 0.5;
    float v = saturate(1.0 - dot(q, q) * 1.55 * c_grade.y * 3.2);
    c *= lerp(1.0, v, saturate(c_grade.y * 3.0));
    return float4(saturate(c), s.a);
}
