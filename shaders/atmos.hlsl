// @shader PS_Atmos PSMain ps_3_0
// Atmosphere pass: screen-space reflections on smooth upward facing surfaces + time-of-day distance fog.
//
// View space is "uv aligned": +x right, +y DOWN, +z forward (the CPU converts everything to it).

sampler2D sColor : register(s0);   // composited scene (point sampled)
sampler2D sDepth : register(s1);   // INTZ depth (point sampled)

float4 c_tex  : register(c0);   // 1/W, 1/H, W, H
float4 c_proj : register(c1);   // m11, m22, m31, -m32
float4 c_dp   : register(c2);   // m33, m43, 0, 0
float4 c_fog  : register(c3);   // fog colour rgb, density
float4 c_fog2 : register(c4);   // start, maxOpacity, sunScatter, fogEnabled
float4 c_sun  : register(c5);   // direction towards the sun (view), sun strength
float4 c_sunc : register(c6);   // sun colour rgb, 0
float4 c_up   : register(c7);   // world up expressed in view space, 0
float4 c_ssr  : register(c8);   // strength, F0, rayLength, thickness
float4 c_ssr2 : register(c9);   // ssrEnabled, frameSeed, 0, 0

float LinZ(float d) { return c_dp.y / (d - c_dp.x); }
float SampleZ(float2 uv) {
    float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    return (d >= 0.99999) ? 1.0e5 : LinZ(d);
}
float3 ViewPos(float2 uv, float z) {
    float2 ndc = uv * 2.0 - 1.0;
    return float3((ndc.x - c_proj.z) * z / c_proj.x, (ndc.y - c_proj.w) * z / c_proj.y, z);
}
float2 ToUV(float3 p) {
    float2 ndc = float2(p.x * c_proj.x / p.z + c_proj.z, p.y * c_proj.y / p.z + c_proj.w);
    return ndc * 0.5 + 0.5;
}
float3 ViewNormal(float2 uv, float3 P) {
    float2 px = c_tex.xy;
    float2 uL = uv - float2(px.x, 0), uR = uv + float2(px.x, 0);
    float2 uU = uv - float2(0, px.y), uD = uv + float2(0, px.y);
    float zL = SampleZ(uL), zR = SampleZ(uR), zU = SampleZ(uU), zD = SampleZ(uD);
    float3 dx = (abs(zR - P.z) < abs(P.z - zL)) ? (ViewPos(uR, zR) - P) : (P - ViewPos(uL, zL));
    float3 dy = (abs(zD - P.z) < abs(P.z - zU)) ? (ViewPos(uD, zD) - P) : (P - ViewPos(uU, zU));
    return normalize(cross(dy, dx));
}
float IGN(float2 pix) { return frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715)))); }
float Lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 PSMain(float2 uv : TEXCOORD0) : COLOR0 {
    float4 src = tex2Dlod(sColor, float4(uv, 0, 0));
    float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    if (d >= 0.99999) return src;           // nothing drawn here (sky / background)

    float z = LinZ(d);
    float3 P = ViewPos(uv, z);
    float3 V = normalize(P);                // camera -> surface
    float3 rgb = src.rgb;

    // ------------------------------------------------------------ screen-space reflections
    if (c_ssr2.x > 0.5) {
        float3 N = ViewNormal(uv, P);
        float floorMask = smoothstep(0.82, 0.96, dot(N, c_up.xyz));
        if (floorMask > 0.01) {
            float3 R = reflect(V, N);
            float cosT = saturate(dot(N, -V));
            float F = c_ssr.y + (1.0 - c_ssr.y) * pow(1.0 - cosT, 5.0);

            // Smooth surfaces (tiles, wood, water) reflect; busy ones (grass, carpet) mostly do not.
            float2 px = c_tex.xy * 2.0;
            float l0 = Lum(rgb);
            float var = abs(Lum(tex2Dlod(sColor, float4(uv + float2(px.x, 0), 0, 0)).rgb) - l0)
                      + abs(Lum(tex2Dlod(sColor, float4(uv - float2(px.x, 0), 0, 0)).rgb) - l0)
                      + abs(Lum(tex2Dlod(sColor, float4(uv + float2(0, px.y), 0, 0)).rgb) - l0)
                      + abs(Lum(tex2Dlod(sColor, float4(uv - float2(0, px.y), 0, 0)).rgb) - l0);
            float flatness = 1.0 - saturate(var * 3.0);
            float amount = c_ssr.x * F * floorMask * lerp(0.2, 1.0, flatness);

            if (amount > 0.002) {
                const int STEPS = 28;
                float jitter = IGN(uv * c_tex.zw + c_ssr2.y);
                float3 origin = P + N * (0.02 + z * 0.002);
                float rayLen = c_ssr.z;
                bool hit = false;
                float2 hitUV = float2(0, 0);
                float hitT = 0.0;
                [loop] for (int i = 0; i < STEPS; ++i) {
                    float t = (i + jitter) / STEPS;
                    t = t * t * 0.8 + t * 0.2;
                    float3 S = origin + R * (t * rayLen);
                    if (S.z < 0.1) break;
                    float2 suv = ToUV(S);
                    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;
                    float sz = SampleZ(suv);
                    float diff = S.z - sz;      // > 0: the ray went behind the visible surface
                    if (diff > 0.03 + S.z * 0.003 && diff < c_ssr.w + t * rayLen * 0.25) {
                        // A ray that travels away from a floor can never strike an upward facing surface from
                        // above. Such "hits" are the ground/floor beyond a lowered wall seen through the depth
                        // buffer, which is what made reflections look like they went through cutaway walls.
                        float3 Nh = ViewNormal(suv, ViewPos(suv, sz));
                        if (dot(Nh, c_up.xyz) <= 0.85) {
                            hit = true; hitUV = suv; hitT = t;
                            break;
                        }
                    }
                }
                float3 sky = c_fog.rgb * 0.55;
                float3 refl = sky;
                float wHit = 0.0;
                if (hit) {
                    float2 e = abs(hitUV * 2.0 - 1.0);
                    wHit = saturate((1.0 - max(e.x, e.y)) * 6.0) * (1.0 - smoothstep(0.35, 1.0, hitT) * 0.9);
                    refl = lerp(sky, tex2Dlod(sColor, float4(hitUV, 0, 0)).rgb, wHit);
                }
                // A ray that escapes (e.g. over the top of a lowered wall) reflects only a faint sky tint.
                rgb = lerp(rgb, refl, saturate(amount * lerp(0.10, 1.0, wHit)));
            }
        }
    }

    // ------------------------------------------------------------ time-of-day distance fog
    if (c_fog2.w > 0.5) {
        float dist = length(P);
        float f = 1.0 - exp(-c_fog.w * max(dist - c_fog2.x, 0.0));
        f = min(f, c_fog2.y);
        float toSun = pow(saturate(dot(V, c_sun.xyz)), 5.0);
        float3 fogCol = c_fog.rgb + c_sunc.rgb * (toSun * c_fog2.z * c_sun.w);
        rgb = lerp(rgb, fogCol, f);
    }
    return float4(rgb, src.a);
}
