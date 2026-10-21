#version 450
// tlg: the trivial shader of the command recording bench (a triangle). It reads 32
// uniform buffers, so the pipeline uses the descriptor set the bench pushes.
layout(set = 0, binding = 0, std140) uniform Buf0 { vec4 data[]; } buf0;
layout(set = 0, binding = 1, std140) uniform Buf1 { vec4 data[]; } buf1;
layout(set = 0, binding = 2, std140) uniform Buf2 { vec4 data[]; } buf2;
layout(set = 0, binding = 3, std140) uniform Buf3 { vec4 data[]; } buf3;
layout(set = 0, binding = 4, std140) uniform Buf4 { vec4 data[]; } buf4;
layout(set = 0, binding = 5, std140) uniform Buf5 { vec4 data[]; } buf5;
layout(set = 0, binding = 6, std140) uniform Buf6 { vec4 data[]; } buf6;
layout(set = 0, binding = 7, std140) uniform Buf7 { vec4 data[]; } buf7;
layout(set = 0, binding = 8, std140) uniform Buf8 { vec4 data[]; } buf8;
layout(set = 0, binding = 9, std140) uniform Buf9 { vec4 data[]; } buf9;
layout(set = 0, binding = 10, std140) uniform Buf10 { vec4 data[]; } buf10;
layout(set = 0, binding = 11, std140) uniform Buf11 { vec4 data[]; } buf11;
layout(set = 0, binding = 12, std140) uniform Buf12 { vec4 data[]; } buf12;
layout(set = 0, binding = 13, std140) uniform Buf13 { vec4 data[]; } buf13;
layout(set = 0, binding = 14, std140) uniform Buf14 { vec4 data[]; } buf14;
layout(set = 0, binding = 15, std140) uniform Buf15 { vec4 data[]; } buf15;
layout(set = 0, binding = 16, std140) uniform Buf16 { vec4 data[]; } buf16;
layout(set = 0, binding = 17, std140) uniform Buf17 { vec4 data[]; } buf17;
layout(set = 0, binding = 18, std140) uniform Buf18 { vec4 data[]; } buf18;
layout(set = 0, binding = 19, std140) uniform Buf19 { vec4 data[]; } buf19;
layout(set = 0, binding = 20, std140) uniform Buf20 { vec4 data[]; } buf20;
layout(set = 0, binding = 21, std140) uniform Buf21 { vec4 data[]; } buf21;
layout(set = 0, binding = 22, std140) uniform Buf22 { vec4 data[]; } buf22;
layout(set = 0, binding = 23, std140) uniform Buf23 { vec4 data[]; } buf23;
layout(set = 0, binding = 24, std140) uniform Buf24 { vec4 data[]; } buf24;
layout(set = 0, binding = 25, std140) uniform Buf25 { vec4 data[]; } buf25;
layout(set = 0, binding = 26, std140) uniform Buf26 { vec4 data[]; } buf26;
layout(set = 0, binding = 27, std140) uniform Buf27 { vec4 data[]; } buf27;
layout(set = 0, binding = 28, std140) uniform Buf28 { vec4 data[]; } buf28;
layout(set = 0, binding = 29, std140) uniform Buf29 { vec4 data[]; } buf29;
layout(set = 0, binding = 30, std140) uniform Buf30 { vec4 data[]; } buf30;
layout(set = 0, binding = 31, std140) uniform Buf31 { vec4 data[]; } buf31;
layout(location = 0) in vec3 in_position;
layout(location = 0) out vec3 v_position;
void main() {
    v_position = in_position + buf0.data[0].xyz;
    gl_Position = vec4(in_position, 1.0);
}
