#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; float2 uv; };

vertex VOut sota_vertex(uint vid [[vertex_id]])
{
    // one triangle covering the viewport
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

// Sharp bilinear: nearest-looking pixels with antialiased edges at any scale.
struct Placement { float2 origin; float2 size; };   // image rectangle in drawable pixels

fragment float4 sota_fragment(VOut in [[stage_in]],
                              texture2d<float> tex [[texture(0)]],
                              constant Placement &pl [[buffer(0)]])
{
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    float2 uv = (in.pos.xy - pl.origin) / pl.size;
    if (any(uv < 0.0) || any(uv >= 1.0)) return float4(0.0, 0.0, 0.0, 1.0);
    float2 size = float2(tex.get_width(), tex.get_height());
    float2 scale = pl.size / size;
    float2 t = uv * size;
    float2 ti = floor(t);
    float2 f = clamp((t - ti - 0.5) * scale + 0.5, 0.0, 1.0);
    return float4(tex.sample(lin, (ti + f) / size).rgb, 1.0);
}
