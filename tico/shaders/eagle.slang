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
vec3 dtt = vec3(65536.0, 255.0, 1.0);
float reduce(vec3 color) { return dot(color, dtt); }

int GET_RESULT(float A, float B, float C, float D) {
    int x = 0, y = 0, r = 0;
    if (A == C) x++; else if (B == C) y++;
    if (A == D) x++; else if (B == D) y++;
    if (x <= 1) r++;
    if (y <= 1) r--;
    return r;
}

void main() {
    vec2 texel = 1.0 / TextureSize;
    float dx = texel.x;
    float dy = texel.y;
    vec2 fp = fract(TexCoord * TextureSize);

    vec3 C0 = texture(Source, TexCoord + vec2(-dx, -dy)).rgb;
    vec3 C1 = texture(Source, TexCoord + vec2(-dx,   0)).rgb;
    vec3 C2 = texture(Source, TexCoord + vec2( dx, -dy)).rgb;
    vec3 D3 = texture(Source, TexCoord + vec2(dx+dx,-dy)).rgb;
    vec3 C3 = texture(Source, TexCoord + vec2(-dx,   0)).rgb;
    vec3 C4 = texture(Source, TexCoord).rgb;
    vec3 C5 = texture(Source, TexCoord + vec2( dx,   0)).rgb;
    vec3 D4 = texture(Source, TexCoord + vec2(dx+dx, 0)).rgb;
    vec3 C6 = texture(Source, TexCoord + vec2(-dx,  dy)).rgb;
    vec3 C7 = texture(Source, TexCoord + vec2(  0,  dy)).rgb;
    vec3 C8 = texture(Source, TexCoord + vec2( dx,  dy)).rgb;
    vec3 D5 = texture(Source, TexCoord + vec2(dx+dx,dy)).rgb;
    vec3 D0 = texture(Source, TexCoord + vec2(-dx,dy+dy)).rgb;
    vec3 D1 = texture(Source, TexCoord + vec2(  0,dy+dy)).rgb;
    vec3 D2 = texture(Source, TexCoord + vec2( dx,dy+dy)).rgb;
    vec3 D6 = texture(Source, TexCoord + vec2(dx+dx,dy+dy)).rgb;

    float c0=reduce(C0); float c1=reduce(C1); float c2=reduce(C2);
    float c3=reduce(C3); float c4=reduce(C4); float c5=reduce(C5);
    float c6=reduce(C6); float c7=reduce(C7); float c8=reduce(C8);
    float d0=reduce(D0); float d1=reduce(D1); float d2=reduce(D2);
    float d3=reduce(D3); float d4=reduce(D4); float d5=reduce(D5);
    float d6=reduce(D6);

    vec3 p00, p10, p01, p11;
    if (c4 != c8) {
        if (c7 == c5) {
            p01 = C7; p10 = C7;
            if ((c6==c7)||(c5==c2)) p00 = 0.25*(3.0*C7+C4);
            else p00 = 0.5*(C4+C5);
            if ((c5==d4)||(c7==d1)) p11 = 0.25*(3.0*C7+C8);
            else p11 = 0.5*(C7+C8);
        } else {
            p11 = 0.125*(6.0*C8+C7+C5);
            p00 = 0.125*(6.0*C4+C7+C5);
            p10 = 0.125*(6.0*C7+C4+C8);
            p01 = 0.125*(6.0*C5+C4+C8);
        }
    } else {
        if (c7 != c5) {
            p11 = C4; p00 = C4;
            if ((c1==c4)||(c8==d5)) p01 = 0.25*(3.0*C4+C5);
            else p01 = 0.5*(C4+C5);
            if ((c8==d2)||(c3==c4)) p10 = 0.25*(3.0*C4+C7);
            else p10 = 0.5*(C7+C8);
        } else {
            int r = 0;
            r += GET_RESULT(c5,c4,c6,d1);
            r += GET_RESULT(c5,c4,c3,c1);
            r += GET_RESULT(c5,c4,d2,d5);
            r += GET_RESULT(c5,c4,c2,d4);
            if (r > 0) { p01=C7; p10=C7; p00=0.5*(C4+C5); p11=0.5*(C4+C5); }
            else if (r < 0) { p11=C4; p00=C4; p01=0.5*(C4+C5); p10=0.5*(C4+C5); }
            else { p11=C4; p00=C4; p01=C7; p10=C7; }
        }
    }
    vec3 res = (fp.x < 0.5) ? (fp.y < 0.5 ? p00 : p10) : (fp.y < 0.5 ? p01 : p11);
    FragColor = vec4(res, 1.0);
}
