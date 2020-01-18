#include "blur.hpp"

static const char* box_vertex_shader =
R"(
#version 100

attribute mediump vec2 position;
uniform vec2 size;
uniform float offset;

varying highp vec2 blurcoord[9];

void main() {
    gl_Position = vec4(position.xy, 0.0, 1.0);

    vec2 texcoord = (position.xy + vec2(1.0, 1.0)) / 2.0;

    blurcoord[0] = texcoord;
    blurcoord[1] = texcoord + vec2(1.0 * offset) / size;
    blurcoord[2] = texcoord - vec2(1.0 * offset) / size;
    blurcoord[3] = texcoord + vec2(2.0 * offset) / size;
    blurcoord[4] = texcoord - vec2(2.0 * offset) / size;
    blurcoord[5] = texcoord + vec2(3.0 * offset) / size;
    blurcoord[6] = texcoord - vec2(3.0 * offset) / size;
    blurcoord[7] = texcoord + vec2(4.0 * offset) / size;
    blurcoord[8] = texcoord - vec2(4.0 * offset) / size;
}
)";

static const char* box_fragment_shader_horz =
R"(
#version 100
precision mediump float;

uniform sampler2D bg_texture;
uniform int mode;

varying highp vec2 blurcoord[9];

void main()
{
    vec2 uv = blurcoord[0];
    vec4 bp = vec4(0.0);
    for(int i = 0; i < 9; i++) {
        vec2 uv = vec2(blurcoord[i].x, uv.y);
        bp += texture2D(bg_texture, uv);
    }

    gl_FragColor = bp / 9.0;
}
)";

static const char* box_fragment_shader_vert =
R"(
#version 100
precision mediump float;

uniform sampler2D bg_texture;
uniform int mode;

varying highp vec2 blurcoord[9];

void main()
{
    vec2 uv = blurcoord[0];
    vec4 bp = vec4(0.0);
    for(int i = 0; i < 9; i++) {
        vec2 uv = vec2(uv.x, blurcoord[i].y);
        bp += texture2D(bg_texture, uv);
    }
    gl_FragColor = bp / 9.0;
}
)";

static const wf_blur_default_option_values box_defaults = {
    .algorithm_name = "box",
    .offset = "2",
    .degrade = "1",
    .iterations = "2"
};

class wf_box_blur : public wf_blur_base
{
    public:
    void get_id_locations(int i)
    {
        }

    wf_box_blur(wf::output_t *output) : wf_blur_base(output, box_defaults)
    {
        OpenGL::render_begin();
        program[0].set_simple(OpenGL::compile_program(
            box_vertex_shader, box_fragment_shader_horz));
        program[1].set_simple(OpenGL::compile_program(
            box_vertex_shader, box_fragment_shader_vert));
        OpenGL::render_end();
    }

    void upload_data(int i, int width, int height)
    {
        float offset = offset_opt;
        static const float vertexData[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
             1.0f,  1.0f,
            -1.0f,  1.0f
        };

        program[i].use(wf::TEXTURE_TYPE_RGBA);
        program[i].uniform2f("size", width, height);
        program[i].uniform1f("offset", offset);
        program[i].attrib_pointer("position", 2, 0, vertexData);
    }

    void blur(int i, int width, int height)
    {
        program[i].use(wf::TEXTURE_TYPE_RGBA);
        render_iteration(fb[i], fb[!i], width, height);
    }

    int blur_fb0(int width, int height) override
    {
        int i, iterations = iterations_opt;

        OpenGL::render_begin();
        GL_CALL(glDisable(GL_BLEND));
        /* Enable our shader and pass some data to it. The shader
         * does box blur on the background texture in two passes,
         * one horizontal and one vertical */
        upload_data(0, width, height);
        upload_data(1, width, height);

        for (i = 0; i < iterations; i++) {
            /* Blur horizontally */
            blur(0, width, height);

            /* Blur vertically */
            blur(1, width, height);
        }

        /* Reset gl state */
        GL_CALL(glEnable(GL_BLEND));
        GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));

        program[0].deactivate();
        GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
        OpenGL::render_end();

        return 0;
    }

    int calculate_blur_radius() override
    {
        return 4 * wf_blur_base::calculate_blur_radius();
    }
};

std::unique_ptr<wf_blur_base> create_box_blur(wf::output_t *output)
{
    return std::make_unique<wf_box_blur> (output);
}
