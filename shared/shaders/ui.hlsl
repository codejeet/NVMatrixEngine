cbuffer Ui : register(b0) {
    float2 viewport;
    float2 translation;
    column_major float4x4 transform;
};
Texture2D image : register(t0);
SamplerState linearClamp : register(s0);
struct Input { float2 position : POSITION; float4 color : COLOR; float2 uv : TEXCOORD; };
struct Pixel { float4 position : SV_Position; float4 color : COLOR; float2 uv : TEXCOORD; };
Pixel VS(Input i) {
    Pixel o;
    float4 p = mul(transform, float4(i.position + translation, 0, 1));
    o.position = float4(2 * p.x / viewport.x - p.w, p.w - 2 * p.y / viewport.y, 0, p.w);
    o.color = i.color; o.uv = i.uv;
    return o;
}
float4 PS(Pixel i) : SV_Target { return i.color * image.Sample(linearClamp, i.uv); }
