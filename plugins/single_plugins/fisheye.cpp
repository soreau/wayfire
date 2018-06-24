#include <plugin.hpp>
#include <output.hpp>
#include <opengl.hpp>
#include <debug.hpp>
#include <animation.hpp>
#include <render-manager.hpp>

static const char* vertex_shader =
R"(
#version 100

attribute mediump vec2 position;

void main() {

    gl_Position = vec4(position.xy, 0.0, 1.0);
}
)";

static const char* fragment_shader =
R"(
#version 100
precision mediump float;

uniform vec2 u_resolution;
uniform vec2 u_mouse;
uniform float u_radius;
uniform float u_zoom;
uniform sampler2D u_texture;

const float PI = 3.1415926535;

void main()
{
        float radius = u_radius;

        float zoom = u_zoom;
        float pw = 1.0 / u_resolution.x;
        float ph = 1.0 / u_resolution.y;

        vec4 p0 = vec4(u_mouse.x, u_resolution.y - u_mouse.y, 1.0 / radius, 0.0);
        vec4 p1 = vec4(pw, ph, PI / radius, (zoom - 1.0) * zoom);
        vec4 p2 = vec4(0, 0, -PI / 2.0, 0.0);

        vec4 t0, t1, t2, t3;

        vec3 tc = vec3(1.0, 0.0, 0.0);
        vec2 uv = vec2(gl_FragCoord.x, gl_FragCoord.y);

        t1 = p0.xyww - vec4(uv, 0.0, 0.0);
        t2.x = t2.y = t2.z = t2.w = 1.0 / sqrt(dot(t1.xyz, t1.xyz));
        t0 = t2 - p0;

        t3.x = t3.y = t3.z = t3.w = 1.0 / t2.x;
        t3 = t3 * p1.z + p2.z;
        t3.x = t3.y = t3.z = t3.w = cos(t3.x);

        t3 = t3 * p1.w;

        t1 = t2 * t1;
        t1 = t1 * t3 + vec4(uv, 0.0, 0.0);

        if (t0.z < 0.0) {
                t1.x = uv.x;
                t1.y = uv.y;
        }

        t1 = t1 * p1 + p2;

        tc = texture2D(u_texture, t1.xy).rgb;

        gl_FragColor = vec4(tc, 1.0);
}
)";

class wayfire_fisheye : public wayfire_plugin_t
{

    post_hook_t hook;
    key_callback toggle_cb;
    wf_duration duration;
    float target_zoom;
    bool active, hook_set;
    wf_option radius, zoom;

    GLuint program, posID, mouseID, resID, radiusID, zoomID;

    public:
        void init(wayfire_config *config)
        {
            auto section = config->get_section("fisheye");
            auto toggle_key = section->get_option("toggle", "<super> KEY_F");
            radius = section->get_option("radius", "300");
            zoom = section->get_option("zoom", "7");

            if (!toggle_key->as_key().valid())
                return;

            target_zoom = zoom->as_double();

            hook = [=] (uint32_t fb, uint32_t tex, uint32_t target)
            {
                render(fb, tex, target);
            };

            toggle_cb = [=] (uint32_t key)
            {
                    if (active)
                    {
                        active = false;
                        duration.start(duration.progress(), 0);
                    } else
                    {
                        active = true;
                        duration.start(duration.progress(), target_zoom);

                        if (!hook_set)
                        {
                            hook_set = true;
                            output->render->add_post(&hook);
                        }
                    }
            };

            auto vs = OpenGL::compile_shader(vertex_shader, GL_VERTEX_SHADER);
            auto fs = OpenGL::compile_shader(fragment_shader, GL_FRAGMENT_SHADER);

            program = GL_CALL(glCreateProgram());
            GL_CALL(glAttachShader(program, vs));
            GL_CALL(glAttachShader(program, fs));
            GL_CALL(glLinkProgram(program));

            posID = GL_CALL(glGetAttribLocation(program, "position"));
            mouseID  = GL_CALL(glGetUniformLocation(program, "u_mouse"));
            resID  = GL_CALL(glGetUniformLocation(program, "u_resolution"));
            radiusID  = GL_CALL(glGetUniformLocation(program, "u_radius"));
            zoomID  = GL_CALL(glGetUniformLocation(program, "u_zoom"));

            duration = wf_duration(new_static_option("700"));
            duration.start(0, 0); // so that the first value we get is correct

            output->add_key(toggle_key, &toggle_cb);
        }

        void render(uint32_t fb, uint32_t tex, uint32_t target)
        {
            GetTuple(x, y, output->get_cursor_position());
            GL_CALL(glUseProgram(program));
            GL_CALL(glBindTexture(GL_TEXTURE_2D, tex));
            GL_CALL(glActiveTexture(GL_TEXTURE0));

            static const float vertexData[] = {
                -1.0f, -1.0f,
                 1.0f, -1.0f,
                 1.0f,  1.0f,
                -1.0f,  1.0f
            };

            auto current_zoom = duration.progress();
            target_zoom = zoom->as_double();

            glUniform2f(mouseID, x, y);
            glUniform2f(resID, output->handle->width, output->handle->height);
            glUniform1f(radiusID, radius->as_double());
            glUniform1f(zoomID, current_zoom);

            GL_CALL(glVertexAttribPointer(posID, 2, GL_FLOAT, GL_FALSE, 0, vertexData));
            GL_CALL(glEnableVertexAttribArray(posID));

            GL_CALL(glDisable(GL_BLEND));
            GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target));
            GL_CALL(glDrawArrays (GL_TRIANGLE_FAN, 0, 4));
            GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));

            GL_CALL(glDisableVertexAttribArray(posID));
            GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));

            if (active)
            {
                /* Reset animation in case target_zoom
                 * was changed via config */
                duration.start(current_zoom, target_zoom);
            } else if (!duration.running())
            {
                output->render->rem_post(&hook);
                hook_set = false;
            }
        }
};

extern "C"
{
    wayfire_plugin_t *newInstance()
    {
        return new wayfire_fisheye();
    }
}
