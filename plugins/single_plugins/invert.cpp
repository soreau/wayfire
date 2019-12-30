#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/debug.hpp>
#include <wayfire/render-manager.hpp>

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
    gl_FragColor = vec4(1.0 - tex_color.r, 1.0 - tex_color.g, 1.0 - tex_color.b, 1.0);
}
)";

class wayfire_invert_screen : public wf::plugin_interface_t
{
    wf::post_hook_t hook;
    wf::activator_callback toggle_cb;

    bool active = false;
    GLuint program, posID, uvID;

  public:
    void load_program()
    {
        OpenGL::render_begin();
        program = OpenGL::compile_program(vertex_shader, fragment_shader);
        posID = GL_CALL(glGetAttribLocation(program, "position"));
        uvID  = GL_CALL(glGetAttribLocation(program, "uvPosition"));
        OpenGL::render_end();
    }

    void init() override
    {
        wf::option_wrapper_t<wf::activatorbinding_t> toggle_key{"invert/toggle"};

        grab_interface->name = "invert";
        grab_interface->capabilities = 0;

        hook = [=] (const wf::framebuffer_base_t& source,
            const wf::framebuffer_base_t& destination) {
            render(source, destination);
        };


        toggle_cb = [=] (wf::activator_source_t, uint32_t) {
            if (!output->can_activate_plugin(grab_interface))
                return false;

            if (active)
            {
                output->render->rem_post(&hook);
            } else
            {
                output->render->add_post(&hook);
            }

            active = !active;

            return true;
        };

        load_program();
        output->add_activator(toggle_key, &toggle_cb);
    }

    void render(const wf::framebuffer_base_t& source,
        const wf::framebuffer_base_t& destination)
    {
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

        OpenGL::render_begin(destination);

        GL_CALL(glUseProgram(program));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, source.tex));
        GL_CALL(glActiveTexture(GL_TEXTURE0));

        GL_CALL(glVertexAttribPointer(posID, 2, GL_FLOAT, GL_FALSE, 0, vertexData));
        GL_CALL(glEnableVertexAttribArray(posID));

        GL_CALL(glVertexAttribPointer(uvID, 2, GL_FLOAT, GL_FALSE, 0, coordData));
        GL_CALL(glEnableVertexAttribArray(uvID));

        GL_CALL(glDisable(GL_BLEND));
        GL_CALL(glDrawArrays (GL_TRIANGLE_FAN, 0, 4));

        GL_CALL(glEnable(GL_BLEND));

        GL_CALL(glDisableVertexAttribArray(posID));
        GL_CALL(glDisableVertexAttribArray(uvID));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
        GL_CALL(glUseProgram(0));

        OpenGL::render_end();
    }

    void fini() override
    {
        if (active)
            output->render->rem_post(&hook);

        OpenGL::render_begin();
        GL_CALL(glDeleteProgram(program));
        OpenGL::render_end();

        output->rem_binding(&toggle_cb);
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_invert_screen);
