#version 440

layout(binding = 0) uniform sampler2D sourceTexture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragmentColor;

void main()
{
    fragmentColor = texture(sourceTexture, uv);
}
