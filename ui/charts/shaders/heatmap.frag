#version 440

layout(binding = 0) uniform sampler2D scalarTexture;
layout(binding = 1) uniform sampler2D paletteTexture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragmentColor;

void main()
{
    float level = texture(scalarTexture, uv).r;
    fragmentColor = texture(paletteTexture, vec2(level, 0.5));
}
