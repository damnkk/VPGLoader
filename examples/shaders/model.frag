#version 450

layout(location = 0) in vec2 texCoord;

layout(binding = 1) uniform sampler2D baseColorTexture;

layout(push_constant) uniform DrawData {
    mat4 model;
    vec4 baseColorFactor;
    float alphaCutoff;
    uint alphaMode;
    vec2 padding;
} drawData;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 color = texture(baseColorTexture, texCoord) * drawData.baseColorFactor;
    if (drawData.alphaMode == 1u && color.a < drawData.alphaCutoff) {
        discard;
    }
    outColor = color;
}
