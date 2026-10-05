#version 450
// Ported from tico's built-in OpenGL shader of the same name.

layout(push_constant) uniform Push
{
    vec4 SourceSize;
    vec4 OutputSize;
} params;

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

#define TexCoord vTexCoord
#define TextureSize params.SourceSize.xy
#define OutputSize params.OutputSize.xy
#define EQ_THRESHOLD 15.0/255.0

float cdist(vec4 A, vec4 B) {
    vec4 diff = A - B;
    return dot(diff, diff);
}
bool is_eq(vec4 A, vec4 B) {
    return cdist(A, B) < EQ_THRESHOLD;
}

void main() {
    vec2 pos = TexCoord * TextureSize;
    vec2 f = fract(pos);
    vec2 dx = vec2(1.0 / TextureSize.x, 0.0);
    vec2 dy = vec2(0.0, 1.0 / TextureSize.y);
    vec4 E = texture(Source, TexCoord);
    vec4 B = texture(Source, TexCoord - dy);
    vec4 D = texture(Source, TexCoord - dx);
    vec4 F = texture(Source, TexCoord + dx);
    vec4 H = texture(Source, TexCoord + dy);
    vec4 res = E;
    bool eLDiff = (B != H && D != F);
    if (eLDiff) {
        if (is_eq(D,B) && !is_eq(D,H) && !is_eq(B,F) && f.x < 0.5 && f.y < 0.5) res = D;
        if (is_eq(B,F) && !is_eq(B,D) && !is_eq(F,H) && f.x >= 0.5 && f.y < 0.5) res = F;
        if (is_eq(D,H) && !is_eq(D,B) && !is_eq(H,F) && f.x < 0.5 && f.y >= 0.5) res = D;
        if (is_eq(H,F) && !is_eq(H,D) && !is_eq(F,B) && f.x >= 0.5 && f.y >= 0.5) res = F;
    }
    FragColor = res;
}
