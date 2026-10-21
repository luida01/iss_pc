#version 450
// tlg: the trivial shader of the command recording bench (a triangle).
layout(location = 0) in vec3 v_position;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = vec4(v_position, 1.0);
}
