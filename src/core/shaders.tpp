static const char *default_vertex_shader_source =
R"(#version 100
attribute mediump vec2 position;
attribute highp vec2 uvPosition;
varying highp vec2 uvpos;

uniform mat4 MVP;

void main() {
    gl_Position = MVP * vec4(position.xy, 0.0, 1.0);
    uvpos = uvPosition;
})";

static const char *default_fragment_shader_source =
R"(#version 100
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform mediump vec4 color;

void main()
{
    mediump vec4 tex_color = texture2D(smp, uvpos);
    tex_color.rgb = tex_color.rgb * color.a;
    gl_FragColor = tex_color * color;
})";
