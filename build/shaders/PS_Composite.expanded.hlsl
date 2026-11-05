// @shader PS_Composite PSMain ps_3_0
// Applies the blurred ambient occlusion and sun shadow to the scene colour.

sampler2D sScene : register(s0);   // copy of the back buffer
sampler2D sAO    : register(s1);   // R = ao, G = sun visibility, B = depth
sampler2D sDepth : register(s2);   // raw depth (debug view)

float4 c_tex  : register(c0);
float4 c_comp : register(c1);   // aoMix, shadowStrength, debugView, 0
float4 c_tint : register(c2);   // shadow tint rgb (cool, bluish), 0

float4 PSMain(float2 uv : TEXCOORD0) : COLOR0 {
    float4 scene = tex2D(sScene, uv);
    float4 aos   = tex2D(sAO, uv);
    float ao = aos.r;
    float sh = aos.g;
    int dbg = (int)(c_comp.z + 0.5);
    if (dbg == 1) return float4(ao.xxx, 1);
    if (dbg == 2) { float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r; return float4(frac(d * 64.0).xxx * 0.5 + saturate(aos.b * 0.01).xxx * 0.5, 1); }
    if (dbg == 3) return float4(sh.xxx, 1);

    float3 col = scene.rgb;
    // AO darkens ambient light; keep a touch of colour bleeding so it does not turn grey
    float aoMix = c_comp.x;
    float3 aoCol = lerp(float3(1, 1, 1), col * 0.55 + 0.45, 0.0);
    col *= lerp(1.0, ao, aoMix);
    // sun shadow: multiply with a cool tint, stronger on bright surfaces
    float lum = dot(col, float3(0.299, 0.587, 0.114));
    float s = (1.0 - sh) * c_comp.y;
    col *= lerp(float3(1, 1, 1), c_tint.rgb, s);
    col *= 1.0 - s * 0.35 * saturate(lum * 1.5);
    return float4(col, scene.a);
}
