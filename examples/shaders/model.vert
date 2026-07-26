#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(binding = 0) uniform CameraData {
    mat4 viewProjection;
} camera;

layout(push_constant) uniform DrawData {
    mat4 model;
    vec4 baseColorFactor;
    float alphaCutoff;
    uint alphaMode;
    vec2 padding;
} drawData;

layout(location = 0) out vec2 texCoord;

void main()
{
    gl_Position = camera.viewProjection * drawData.model * vec4(inPosition, 1.0);
    texCoord = inTexCoord;
}
