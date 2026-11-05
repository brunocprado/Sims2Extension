// @shader PS_AO PSMain ps_3_0
// Ground-truth style ambient occlusion (horizon based, cosine weighted) + screen-space sun contact shadows.
// Output: R = ambient visibility, G = sun visibility, B = view-space depth (for the bilateral blur), A = 1.
//
// View space used here is "uv aligned": +x right, +y DOWN, +z forward. The CPU already converts the sun
// direction and the projection constants to this convention.

sampler2D sDepth : register(s0);  // INTZ depth texture, point sampled

float4 c_tex  : register(c0);  // 1/W, 1/H, W, H
float4 c_proj : register(c1);  // m11, m22, m31, -m32   (D3D row-vector projection terms)
float4 c_dp   : register(c2);  // m33, m43, 0, 0
float4 c_ao   : register(c3);  // radius, intensity, power, aoEnabled
float4 c_sun  : register(c4);  // sun direction towards the light (view space), shadowStrength
float4 c_sh   : register(c5);  // shadowLength, shadowEnabled, frameSeed, thickness

static const float PI = 3.14159265;

float LinZ(float d) {
    // d >= 1 is the cleared far plane (sky / nothing drawn)
    float z = c_dp.y / (d - c_dp.x);
    return (d >= 0.99999) ? 1.0e5 : z;
}

float SampleZ(float2 uv) {
    return LinZ(tex2Dlod(sDepth, float4(uv, 0, 0)).r);
}

float3 ViewPos(float2 uv, float z) {
    float2 ndc = uv * 2.0 - 1.0;
    return float3((ndc.x - c_proj.z) * z / c_proj.x,
                  (ndc.y - c_proj.w) * z / c_proj.y, z);
}

float2 ToUV(float3 p) {
    float2 ndc = float2(p.x * c_proj.x / p.z + c_proj.z,
                        p.y * c_proj.y / p.z + c_proj.w);
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

float IGN(float2 pix) {
    return frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))));
}

float4 PSMain(float2 uv : TEXCOORD0) : COLOR0 {
    float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    if (d >= 0.99999) return float4(1, 1, 1000.0, 1);

    float  z = LinZ(d);
    float3 P = ViewPos(uv, z);
    float3 N = ViewNormal(uv, P);
    float3 V = normalize(-P);
    float2 pix = uv * c_tex.zw;
    float noise  = IGN(pix + c_sh.z);
    float noise2 = frac(noise * 17.0 + 0.618);

    // ---------------------------------------------------------------- ambient occlusion
    float vis = 1.0;
    if (c_ao.w > 0.5) {
        const int SLICES = 3;
        const int STEPS  = 7;
        float R = c_ao.x;
        float pxPerUnit = 0.5 * c_tex.z * c_proj.x / z;
        float radiusPx  = clamp(R * pxPerUnit, 5.0, 180.0);
        float acc = 0.0;
        [loop] for (int s = 0; s < SLICES; ++s) {
            float phi = (s + noise) * (PI / SLICES);
            float2 omega = float2(cos(phi), -sin(phi));
            float3 dir3 = float3(omega, 0.0);
            float3 orthoDir = dir3 - dot(dir3, V) * V;
            float3 axis = normalize(cross(dir3, V));
            float3 projN = N - axis * dot(N, axis);
            float projLen = max(length(projN), 1.0e-4);
            float sgn = sign(dot(orthoDir, projN));
            float cosN = saturate(dot(projN, V) / projLen);
            float n = sgn * acos(cosN);

            float hc0 = -1.0, hc1 = -1.0;
            [loop] for (int i = 0; i < STEPS; ++i) {
                float t = (i + noise2) / STEPS;
                float offPx = t * t * radiusPx + 1.5;
                float2 o = omega * offPx * c_tex.xy;
                float3 S0 = ViewPos(uv + o, SampleZ(uv + o));
                float3 S1 = ViewPos(uv - o, SampleZ(uv - o));
                float3 d0 = S0 - P, d1 = S1 - P;
                float l0 = length(d0), l1 = length(d1);
                float w0 = saturate((R - l0) / (R * 0.45));
                float w1 = saturate((R - l1) / (R * 0.45));
                hc0 = max(hc0, lerp(-1.0, dot(d0, V) / max(l0, 1.0e-4), w0));
                hc1 = max(hc1, lerp(-1.0, dot(d1, V) / max(l1, 1.0e-4), w1));
            }
            float h0 = -acos(clamp(hc1, -1.0, 1.0));
            float h1 =  acos(clamp(hc0, -1.0, 1.0));
            h0 = n + clamp(h0 - n, -PI * 0.5, PI * 0.5);
            h1 = n + clamp(h1 - n, -PI * 0.5, PI * 0.5);
            float sn = sin(n);
            acc += projLen * 0.25 * ((cosN + 2.0 * h0 * sn - cos(2.0 * h0 - n)) +
                                     (cosN + 2.0 * h1 * sn - cos(2.0 * h1 - n)));
        }
        vis = saturate(acc / SLICES);
        vis = pow(saturate(1.0 - (1.0 - vis) * c_ao.y), c_ao.z);
    }

    // ---------------------------------------------------------------- sun contact shadow
    float sunVis = 1.0;
    if (c_sh.y > 0.5) {
        const int RAY = 20;
        float3 L = normalize(c_sun.xyz);
        float facing = saturate(dot(N, L));            // surfaces turned away are already dark
        float3 origin = P + N * (0.02 + z * 0.0015);
        float len = c_sh.x;
        float occ = 0.0;
        [loop] for (int r = 0; r < RAY; ++r) {
            float t = (r + noise) / RAY;
            t = t * t * 0.85 + t * 0.15;
            float3 S = origin + L * (t * len);
            float2 suv = ToUV(S);
            if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;
            float sz = SampleZ(suv);
            float diff = S.z - sz;                      // > 0: ray point is behind the visible surface
            float thick = c_sh.w + t * len * 0.35;
            float hit = (diff > 0.03 + S.z * 0.002 && diff < thick) ? 1.0 : 0.0;
            occ = max(occ, hit * (1.0 - t * 0.55));
        }
        sunVis = 1.0 - occ;
        // lit side only: a surface facing away from the sun is not "shadowed" a second time
        sunVis = lerp(1.0, sunVis, saturate(facing * 3.0 + 0.25));
    }

    return float4(vis, sunVis, z, 1.0);
}
