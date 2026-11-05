// @shader PS_DofFocus PSFocus ps_3_0
// @shader PS_Dof PSDof ps_3_0
// Depth of field.
//   PSFocus: 1x1 target. Reads the depth around the screen centre and eases the focus distance towards it
//            (previous value in sPrev), so the focus glides instead of jumping.
//   PSDof:   gather blur. Circle of confusion from depth (or a tilt-shift band), golden-angle disc kernel, samples
//            weighted by their own CoC so sharp in-focus pixels do not bleed into the blur.

sampler2D sColor : register(s0);   // scene (point sampled)
sampler2D sDepth : register(s1);   // INTZ depth (point sampled)
sampler2D sPrev  : register(s2);   // previous focus distance (1x1, point sampled)

float4 c_tex  : register(c0);   // 1/W, 1/H, W, H
float4 c_dp   : register(c1);   // m33, m43, 0, 0
float4 c_dof  : register(c2);   // manual focus distance, blur strength (px per unit of relative depth error), max radius px, auto focus (0/1)
float4 c_dof2 : register(c3);   // easing speed, tilt-shift (0/1), tilt band half height (0..0.5), dead zone (relative depth)

float LinZ(float d) { return c_dp.y / (d - c_dp.x); }
float SampleZ(float2 uv) {
    float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    return (d >= 0.99999) ? 400.0 : LinZ(d);
}
float IGN(float2 pix) { return frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715)))); }

float4 PSFocus(float2 uv : TEXCOORD0) : COLOR0 {
    // median-ish of 9 taps near the centre: a thin object under the exact centre does not yank the focus
    float z[9];
    int k = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            z[k++] = SampleZ(float2(0.5, 0.5) + float2(x, y) * 0.035);
        }
    float lo = z[0], hi = z[0], sum = z[0];
    [unroll] for (int i = 1; i < 9; ++i) { lo = min(lo, z[i]); hi = max(hi, z[i]); sum += z[i]; }
    float target = (sum - lo - hi) / 7.0;
    target = clamp(target, 0.5, 300.0);
    float prev = tex2Dlod(sPrev, float4(0.5, 0.5, 0, 0)).r;
    float f = (prev > 0.1 && prev < 1000.0) ? lerp(prev, target, c_dof2.x) : target;
    return float4(f, 0, 0, 1);
}

// Signed circle of confusion in pixels: > 0 behind the focus plane, < 0 in front of it.
float CoC(float2 uv, float z, float focus) {
    if (c_dof2.y > 0.5) {
        float band = c_dof2.z;
        float e = (abs(uv.y - 0.5) - band) / max(0.5 - band, 0.05);
        return saturate(e) * c_dof.z;
    }
    float rel = (z - focus) / max(z, 0.5);
    float dead = c_dof2.w;
    float m = sign(rel) * max(abs(rel) - dead, 0.0);
    return clamp(m * c_dof.y, -c_dof.z, c_dof.z);
}

float4 PSDof(float2 uv : TEXCOORD0) : COLOR0 {
    float4 src = tex2Dlod(sColor, float4(uv, 0, 0));
    float focus = (c_dof.w > 0.5) ? tex2Dlod(sPrev, float4(0.5, 0.5, 0, 0)).r : c_dof.x;
    float z0 = SampleZ(uv);
    float coc0 = CoC(uv, z0, focus);
    float r0 = abs(coc0);
    if (r0 < 0.6) {
        // in focus here, but a blurred foreground object next to us may still spill over: check a few neighbours
        float spill = 0.0;
        [unroll] for (int j = 0; j < 4; ++j) {
            float a = j * 1.5708 + 0.7;
            float2 o = float2(cos(a), sin(a)) * c_dof.z * 0.5 * c_tex.xy;
            float2 su = uv + o;
            float zs = SampleZ(su);
            float cs = CoC(su, zs, focus);
            spill = max(spill, (cs < 0.0 ? -cs : 0.0));
        }
        if (spill < 1.5) return src;
        r0 = spill * 0.5;
    }

    const int N = 28;
    float rot = IGN(uv * c_tex.zw) * 6.2831853;
    float3 acc = src.rgb;
    float wsum = 1.0;
    [loop] for (int i = 0; i < N; ++i) {
        float t = (i + 0.5) / N;
        float rad = sqrt(t) * r0;
        float a = i * 2.3999632 + rot;
        float2 o = float2(cos(a), sin(a)) * rad;
        float2 su = uv + o * c_tex.xy;
        float3 c = tex2Dlod(sColor, float4(su, 0, 0)).rgb;
        float zs = SampleZ(su);
        float cs = CoC(su, zs, focus);
        // a sample contributes only when its own blur disc reaches us, so sharp neighbours never bleed into a blurred pixel
        float reach = saturate(abs(cs) - rad + 1.0);
        float w = reach;
        // brighter samples bloom a little into the bokeh
        w *= 1.0 + 0.35 * saturate(max(c.r, max(c.g, c.b)) - 0.8);
        acc += c * w;
        wsum += w;
    }
    return float4(acc / wsum, src.a);
}
