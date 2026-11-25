// Replaces the game's water shaders (dust/WorldViewer_Water.fx, RenderWaterDeferred and RenderWater).
// ENHANCED 0 gives the original result.
#ifndef ENHANCED
#define ENHANCED 1
#endif

sampler2D sParams : register(s0);    // z foam, w sediment
sampler2D sScroll : register(s1);    // two sets of texture coordinates carried along by the flow
sampler2D sRamp : register(s3);      // colour by light and depth
sampler2D sNormal : register(s4);    // zw slope of the water surface
sampler2D sWaterZ : register(s5);    // distance of the water, 1000 where there is none
sampler2D sCommon : register(s6);
samplerCUBE sCube : register(s8);
sampler2D sRipple : register(s9);    // xy slope of the ripples, w foam
sampler2D sScene : register(s10);    // what is behind the water
sampler2D sClouds : register(s14);
sampler2D sDepth : register(s15);    // w distance of what is behind the water

float4 cTime : register(c36);        // x mix of the two coordinate sets
float4 cSediment : register(c37);
float4 cScroll : register(c38);      // z tiling, w foam
float4 cNormal : register(c40);      // z tiling
float4 cDetail : register(c42);      // zw fade at the shore
float4 cView[3] : register(c50);     // view to world
float4 cClouds[2] : register(c65);

// ---- common ---------------------------------------------------------------------------------------
struct Surface {
    float3 pos;      // world
    float3 eye;      // camera to surface, normalised
    float3 sun;      // towards the sun
    float2 map;      // place on the map, 0..1
    float2 screen;   // place on screen, 0..1
    float dist;      // view depth
};

// Bilinear, the texture does not support filtering.
float4 Scroll(float2 uv) {
    const float h = 0.5 / 512;
    float2 a = uv - h;
    float2 f = frac(a * 512);
    float4 s00 = tex2D(sScroll, a), s11 = tex2D(sScroll, uv + h);
    float4 s10 = tex2D(sScroll, a + float2(2 * h, 0)), s01 = tex2D(sScroll, a + float2(0, 2 * h));
    float4 s = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
    float2 e = 1 - abs(1 - 2 * saturate(uv));
    float inside = saturate(e.x * e.y * 16 - 1);   // not near the borders of the map
    return lerp(uv.xyxy, s, inside);
}

// Cubic (B-spline) filtering in four taps.
float4 Smooth(sampler2D s, float2 uv) {
    float2 p = uv * 512 - 0.5;
    float2 f = frac(p);
    p -= f;
    float2 f2 = f * f, f3 = f2 * f;
    float2 w0 = (1 - f) * (1 - f) * (1 - f) / 6, w1 = (4 - 6 * f2 + 3 * f3) / 6;
    float2 w3 = f3 / 6, w2 = 1 - w0 - w1 - w3;
    float2 g0 = w0 + w1, g1 = w2 + w3;
    float2 t0 = (p - 0.5 + w1 / g0) / 512, t1 = (p + 1.5 + w3 / g1) / 512;
    return (tex2Dlod(s, float4(t0, 0, 0)) * g0.x + tex2Dlod(s, float4(t1.x, t0.y, 0, 0)) * g1.x) * g0.y +
           (tex2Dlod(s, float4(t0.x, t1.y, 0, 0)) * g0.x + tex2Dlod(s, float4(t1, 0, 0)) * g1.x) * g1.y;
}

// Screen space reflection along uv0..uv1, depth w0..w1. Alpha is the confidence.
#define STEPS 18
float4 Trace(float2 uv0, float w0, float2 uv1, float w1, float jitter) {
    float2 uv = uv0;
    float hit = 0, last = 0, t = 0, q0 = 1 / w0, q1 = 1 / w1;
    [loop] for (int i = 0; i < STEPS; i++) {
        float k = (i + jitter) / STEPS;
        t = k * (0.25 + 0.75 * k) + 0.004;
        float w = 1 / lerp(q0, q1, t), before = 1 / lerp(q0, q1, last);
        float d = tex2Dlod(sDepth, float4(lerp(uv0, uv1, t), 0, 0)).w;
        if (d > 0.5 && w > d + 0.05 && w - d < 6 + 2 * abs(w - before)) { hit = 1; break; }
        last = t;
    }
    if (hit > 0) {
        float lo = last, hi = t;
        for (int j = 0; j < 5; j++) {
            float m = 0.5 * (lo + hi);
            float wm = 1 / lerp(q0, q1, m), dm = tex2Dlod(sDepth, float4(lerp(uv0, uv1, m), 0, 0)).w;
            if (wm > dm + 0.05) hi = m; else lo = m;
        }
        t = hi;
        uv = lerp(uv0, uv1, t);
        float wh = 1 / lerp(q0, q1, t), dh = tex2Dlod(sDepth, float4(uv, 0, 0)).w;
        float2 e = min(uv, 1 - uv);
        hit = saturate(min(e.x, e.y) * 12) * saturate((1 - t) * 4) * saturate(2 - abs(wh - dh) * 0.5);
        hit *= step(dh, tex2Dlod(sWaterZ, float4(uv, 0, 0)).x);   // nothing under water
    }
    return float4(tex2Dlod(sScene, float4(uv, 0, 0)).rgb, hit);
}

struct Water {
    float3 n, level, mirror;
    float ripple, foam, ndl;
};

Water Waves(Surface s) {
    Water o;
    float4 flow = Scroll(s.map) * (cScroll.z * cNormal.z);
    float4 a = tex2D(sRipple, flow.zw), b = tex2D(sRipple, flow.xy);
    float3 t = lerp(a, b, cTime.x).xyw;
    float2 ripple = (4 * t.xy - 3) * 0.125 + 0.5;
#if ENHANCED
    // Finer ripples: the difference of two layers, without slope of its own. Faded out where
    // smaller than a pixel.
    float2 px = max(abs(ddx(s.map)), abs(ddy(s.map))) * abs(cScroll.z * cNormal.z) * 3.3 * 256;
    float near = saturate(1.6 - max(px.x, px.y));
    float3 f0 = lerp(tex2Dlod(sRipple, float4(flow.zw * 2.6, 0, 0)), tex2Dlod(sRipple, float4(flow.xy * 2.6, 0, 0)), cTime.x).xyw;
    float3 f1 = lerp(tex2Dlod(sRipple, float4(flow.wz * 3.3 + 0.37, 0, 0)), tex2Dlod(sRipple, float4(flow.yx * 3.3 + 0.37, 0, 0)), cTime.x).xyw;
    float3 fine = (f0 - f1) * near;
    ripple += fine.xy * 0.3;
    float2 big = Smooth(sNormal, s.map).zw;
#else
    float2 big = tex2D(sNormal, s.map).zw;
#endif
    o.n = normalize(float3(2 * big + ripple - 1, 1));
    o.foam = t.z * cScroll.w;
#if ENHANCED
    o.ripple = fine.z;
    // Still water has a slope of 0.25, removed for reflection and glitter.
    o.level = normalize(float3(2 * big + ripple - 1 - 0.25, 1));
    float3 m = normalize(float3(o.level.xy * 0.55, o.level.z));
    o.mirror = reflect(s.eye, m);
    o.mirror.z = max(o.mirror.z, 0.02);
#else
    o.ripple = 0;
    o.level = o.n;
    o.mirror = 0;
#endif
    o.ndl = saturate(dot(o.n, s.sun));
    return o;
}

float4 Shade(Surface s, Water o, float4 seen) {
    float3 n = o.n;
    float3 cube = texCUBE(sCube, reflect(s.eye, n)).rgb;
    float2 bend = (n.xy - 0.25) * tex2D(sCommon, s.map).y * 0.25;
#if ENHANCED
    float4 params = Smooth(sParams, s.map);
#else
    float4 params = tex2D(sParams, s.map);
#endif
    float behindBent = tex2D(sDepth, s.screen + bend).w, behind = tex2D(sDepth, s.screen).w;
    float deep = (max(behind, behindBent) - s.dist) * 0.025 - 0.01;
    float alpha = saturate(((behind - s.dist) * 0.025 - 0.01 - cDetail.z) * cDetail.w);
    float2 at = s.screen + bend * saturate(deep);
    float3 through = tex2D(sScene, at).rgb, straight = tex2D(sScene, s.screen).rgb;
    float wz = tex2D(sWaterZ, at).x;
    through = wz >= 990 ? straight : through;
#if ENHANCED
    float at_ = tex2D(sDepth, at).w;
    through = at_ < s.dist ? straight : through;            // in front of the water
#endif
    float cloud = dot(tex2D(sClouds, s.map * cClouds[1].zw + cClouds[1].xy), cClouds[0]);
    float light = o.ndl * 0.5 - cloud + 0.75;
    float4 ramp = tex2Dlod(sRamp, float4(light, 1 - deep, 0, 0));
    float3 mud = saturate(light * cSediment.rgb + 0.35);
    float3 c = lerp(cube + ramp.rgb, mud, params.w);
    c = lerp(through, c, saturate(params.w + ramp.a));
    float foam = saturate(o.foam * params.z);
#if ENHANCED
    float facing = saturate(dot(o.level, -s.eye));
    float glance = (1 - facing) * (1 - facing) * (1 - facing);
    float clear = 1 - 0.6 * saturate(params.w);
    c += cube * (glance * 0.35 * clear);
    c = lerp(c, seen.rgb, seen.a * clear * min(0.06 + 0.94 * glance, 0.85));
    float sun = saturate(dot(reflect(s.eye, o.level), normalize(s.sun)));
    float glint = pow(sun, 260) * 0.6 + pow(sun, 40) * 0.04;
    c += float3(1, 0.93, 0.8) * (glint * clear * saturate(1 - 2 * cloud));
    foam = lerp(foam, smoothstep(0.06, 0.8, foam), 0.7);
    foam = saturate(foam + foam * (1 - foam) * 2.5 * o.ripple);
    alpha = alpha * alpha * (3 - 2 * alpha);
#endif
    return float4(lerp(c, 1, foam), alpha);
}

float Jitter(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
#define REACH 150

// ---- deferred, full screen -------------------------------------------------------------------
float4 Deferred(float2 uv : TEXCOORD0, float3 ray : TEXCOORD1, float3 eye : TEXCOORD2, float4 map : TEXCOORD3, float3 sun : TEXCOORD4
#if ENHANCED
                , float2 vpos : VPOS
#endif
                ) : COLOR {
    Surface s;
    s.screen = uv;
    s.dist = tex2D(sWaterZ, uv).x;
    float4 v = float4(s.dist * ray, 1);
    s.pos = float3(dot(v, cView[0]), dot(v, cView[1]), dot(v, cView[2]));
    s.eye = normalize(s.pos - eye);
    s.sun = sun;
    s.map = (s.pos.xy * (2.0 / 3) + map.xy) * map.zw;
    Water o = Waves(s);
    float4 seen = 0;
#if ENHANCED
    float2 scale = float2(ddx(uv.x) / ddx(ray.x), ddy(uv.y) / ddy(ray.y));
    float3 r = float3(dot(o.mirror, float3(cView[0].x, cView[1].x, cView[2].x)), dot(o.mirror, float3(cView[0].y, cView[1].y, cView[2].y)),
                      dot(o.mirror, float3(cView[0].z, cView[1].z, cView[2].z)));
    float reach = r.z < 0 ? min(REACH, max(s.dist - 2, 0.01) / -r.z) : REACH;
    float3 q = v.xyz + r * reach;
    float2 uv1 = uv + (q.xy / q.z - ray.xy) * scale;
    seen = Trace(uv, s.dist, uv1, q.z, Jitter(vpos));
    seen.a *= saturate(r.z * 2 + 1.4);
#endif
    return Shade(s, o, seen);
}

// ---- forward ---------------------------------------------------------------------------------
struct VSC { float4 world[4]; float4 vp[4]; float4 wvp[4]; float4 border; float4 sun; float4 add; float4 proj; float4 view; };
VSC vs : register(c20);
float4 vsPlane : register(c64);
struct V2P {
    float4 pos : POSITION;
    float4 cell : TEXCOORD0;
    float4 screen : TEXCOORD1;
    float3 eye : TEXCOORD3;
    float4 sun : TEXCOORD4;
    float4 world : TEXCOORD5;
    float4 u : TEXCOORD6;        // world to screen
    float4 v : TEXCOORD7;
    float4 w : TEXCOORD8;
};
V2P ForwardVS(float4 p : POSITION) {
    V2P o;
    float4 w = float4(p.xy * 1.5, p.z + vsPlane.x, 1);
    float4 c = float4(dot(w, vs.vp[0]), dot(w, vs.vp[1]), dot(w, vs.vp[2]), dot(w, vs.vp[3]));
    o.pos = c;
    o.cell.xy = p.xy + vs.add.xy;
    o.cell.zw = o.cell.xy * vs.add.zw;
    o.screen = float4(c.xy * vs.proj.xy + c.w * vs.proj.zw, c.zw);
    o.eye = w.xyz - vs.view.xyz;
    o.sun = vs.sun;
    o.world = w;
    o.u = vs.vp[0] * vs.proj.x + vs.vp[3] * vs.proj.z;
    o.v = vs.vp[1] * vs.proj.y + vs.vp[3] * vs.proj.w;
    o.w = vs.vp[3];
    return o;
}
float4 Forward(float4 screen : TEXCOORD1, float3 eye : TEXCOORD3, float4 sun : TEXCOORD4, float4 world : TEXCOORD5
#if ENHANCED
               , float4 ru : TEXCOORD6, float4 rv : TEXCOORD7, float4 rw : TEXCOORD8, float2 vpos : VPOS
#endif
               ) : COLOR {
    Surface s;
    s.dist = screen.w;
    s.screen = screen.xy / screen.w;
    s.pos = world.xyz;
    s.eye = normalize(eye);
    s.sun = sun.xyz;
    s.map = world.xy / 768 + 0.5;
    Water o = Waves(s);
    float4 seen = 0;
#if ENHANCED
    float along = dot(o.mirror, rw.xyz);
    float reach = along < 0 ? min(REACH, max(s.dist - 2, 0.01) / -along) : REACH;
    float4 q = float4(world.xyz + o.mirror * reach, 1);
    float w1 = dot(q, rw);
    seen = Trace(s.screen, s.dist, float2(dot(q, ru), dot(q, rv)) / w1, w1, Jitter(vpos));
    seen.a *= saturate(along * 2 + 1.4);
#endif
    return Shade(s, o, seen);
}
