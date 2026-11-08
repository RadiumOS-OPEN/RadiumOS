#version 450
layout(location=0) in vec3 vColor;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PC { float t; } pc;
void main() {
    float k = 0.75 + 0.25 * sin(pc.t * 3.0 + gl_FragCoord.x * 0.05);
    outColor = vec4(vColor * k, 1.0);
}
