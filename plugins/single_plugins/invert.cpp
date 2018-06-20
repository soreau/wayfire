#include <plugin.hpp>
#include <output.hpp>
#include <opengl.hpp>
#include <debug.hpp>
#include <render-manager.hpp>

static const char* vertex_shader =
R"(
#version 100

attribute mediump vec2 position;
attribute highp vec2 uvPosition;

varying highp vec2 uvpos;

void main() {

    gl_Position = vec4(position.xy, 0.0, 1.0);
    uvpos = uvPosition;
}
)";

static const char* fragment_shader =
R"(
#version 100

varying highp vec2 uvpos;
uniform sampler2D smp;

void main()
{
    mediump vec4 tex_color = texture2D(smp, uvpos);
    gl_FragColor = vec4(1.0 - tex_color.r, 1.0 - tex_color.g, 1.0 - tex_color.b, 1.0f);
}
)";

class wayfire_invert_screen : public wayfire_plugin_t
{

    post_hook_t hook;
    effect_hook_t wait_for_render;

    GLuint program, posID, uvID;

    public:
        void init(wayfire_config *config)
        {
            hook = [=] (uint32_t fb, uint32_t tex, uint32_t target)
            {
                render(fb, tex, target);
            };

            wait_for_render = [=] () {
                output->render->add_post(&hook);
                output->render->rem_effect(&wait_for_render, WF_OUTPUT_EFFECT_POST);

                auto vs = OpenGL::compile_shader(vertex_shader, GL_VERTEX_SHADER);
                auto fs = OpenGL::compile_shader(fragment_shader, GL_FRAGMENT_SHADER);

                program = GL_CALL(glCreateProgram());
                GL_CALL(glAttachShader(program, vs));
                GL_CALL(glAttachShader(program, fs));
                GL_CALL(glLinkProgram(program));

                posID = GL_CALL(glGetAttribLocation(program, "position"));
                uvID  = GL_CALL(glGetAttribLocation(program, "uvPosition"));
            };

            output->render->add_effect(&wait_for_render, WF_OUTPUT_EFFECT_POST);
        }

        void render(uint32_t fb, uint32_t tex, uint32_t target)
        {
            GL_CALL(glUseProgram(program));
            GL_CALL(glBindTexture(GL_TEXTURE_2D, tex));
            GL_CALL(glActiveTexture(GL_TEXTURE0));

            static const float vertexData[] = {
                -1.0f, -1.0f,
                 1.0f, -1.0f,
                 1.0f,  1.0f,
                -1.0f,  1.0f
            };

            static const float coordData[] = {
                0.0f, 0.0f,
                1.0f, 0.0f,
                1.0f, 1.0f,
                0.0f, 1.0f
            };

            GL_CALL(glVertexAttribPointer(posID, 2, GL_FLOAT, GL_FALSE, 0, vertexData));
            GL_CALL(glEnableVertexAttribArray(posID));

            GL_CALL(glVertexAttribPointer(uvID, 2, GL_FLOAT, GL_FALSE, 0, coordData));
            GL_CALL(glEnableVertexAttribArray(uvID));

            GL_CALL(glDisable(GL_BLEND));
            GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target));
            GL_CALL(glDrawArrays (GL_TRIANGLE_FAN, 0, 4));
            GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));

            GL_CALL(glDisableVertexAttribArray(posID));
            GL_CALL(glDisableVertexAttribArray(uvID));
            GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
        }
};

extern "C"
{
    wayfire_plugin_t *newInstance()
    {
        return new wayfire_invert_screen();
    }
}
