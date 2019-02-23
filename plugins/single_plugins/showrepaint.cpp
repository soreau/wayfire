/*
 * The MIT License (MIT)
 * 
 * Copyright (c) 2019 Scott Moreau
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "plugin.hpp"
#include "output.hpp"
#include "opengl.hpp"
#include "debug.hpp"
#include "render-manager.hpp"

static const char* vertex_shader =
R"(
#version 100

attribute mediump vec2 position;

void main() {

    gl_Position = vec4(position, 0.0, 1.0);
}
)";

static const char* fragment_shader =
R"(
#version 100
precision mediump float;

uniform vec4 color;

void main()
{
    gl_FragColor = color;
}
)";

class wayfire_showrepaint : public wayfire_plugin_t
{

    effect_hook_t pre_hook, overlay_hook;
    activator_callback toggle_cb;
    bool active, hook_set;
    wf_region damage, last_damage, ll_damage;
    float last_color[4], ll_color[4];

    GLuint program, posID, colorID;

    void load_program()
    {
        OpenGL::render_begin();
        program = OpenGL::create_program_from_source(
            vertex_shader, fragment_shader);

        posID = GL_CALL(glGetAttribLocation(program, "position"));
        colorID  = GL_CALL(glGetUniformLocation(program, "color"));

        OpenGL::render_end();
    }

    public:
        void init(wayfire_config *config)
        {
            auto section = config->get_section("showrepaint");
            auto toggle_key = section->get_option("toggle", "<ctrl> KEY_S");

            active = false;

            toggle_cb = [=] (wf_activator_source, uint32_t)
            {
                active = !active;
            };
            output->add_activator(toggle_key, &toggle_cb);

            pre_hook = [=] () { get_damage(); };
            overlay_hook = [=] () { render(); };

            output->render->add_effect(&pre_hook, WF_OUTPUT_EFFECT_PRE);
            output->render->add_effect(&overlay_hook, WF_OUTPUT_EFFECT_OVERLAY);

            load_program();

            ll_color[3] = last_color[3] = 0.0;
        }

        void get_damage()
        {
            damage = output->render->get_scheduled_damage();
        }

        void render()
        {
            wf_framebuffer target_fb = output->render->get_target_framebuffer();

            float r = 0.25 + static_cast <float> (rand()) /( static_cast <float> (RAND_MAX/(0.5)));
            float g = 0.25 + static_cast <float> (rand()) /( static_cast <float> (RAND_MAX/(0.5)));
            float b = 0.25 + static_cast <float> (rand()) /( static_cast <float> (RAND_MAX/(0.5)));
            float color[4] = {r, g, b, 0.25};

            wf_region last_region = last_damage ^ damage;
            wf_region ll_region = (ll_damage ^ last_damage) ^ damage;

            for (const auto& box : ll_region)
                render_box(target_fb, box, ll_color);

            for (const auto& box : last_region)
                render_box(target_fb, box, last_color);

            for (const auto& box : damage)
                render_box(target_fb, box, color);

            memcpy(ll_color, last_color, sizeof (float) * 4);
            memcpy(last_color, color, sizeof (float) * 4);
            ll_damage = last_damage;
            last_damage = damage;
        }

        void render_box(const wf_framebuffer& target_fb, pixman_box32_t box, float color[4])
        {
            if (!active)
                return;

            OpenGL::render_begin(target_fb);

            static const float vertexData[] = {
                -1.0f, -1.0f,
                 1.0f, -1.0f,
                 1.0f,  1.0f,
                -1.0f,  1.0f
            };

            GL_CALL(glUseProgram(program));
            GL_CALL(glEnableVertexAttribArray(posID));

            GL_CALL(glVertexAttribPointer(posID, 2, GL_FLOAT, GL_FALSE, 0, vertexData));
            GL_CALL(glUniform4fv(colorID, 1, &color[0]));

            target_fb.bind();
            wlr_box scissor_box = wlr_box_from_pixman_box(box);
            target_fb.scissor(scissor_box);
            
            GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
            
            /* Disable stuff */
            GL_CALL(glUseProgram(0));
            GL_CALL(glDisableVertexAttribArray(posID));
            
            OpenGL::render_end();
        }

        void fini()
        {
            OpenGL::render_begin();
            GL_CALL(glDeleteProgram(program));
            OpenGL::render_end();

            output->rem_binding(&toggle_cb);
            output->render->rem_effect(&pre_hook, WF_OUTPUT_EFFECT_PRE);
            output->render->rem_effect(&overlay_hook, WF_OUTPUT_EFFECT_OVERLAY);
        }
};

extern "C"
{
    wayfire_plugin_t *newInstance()
    {
        return new wayfire_showrepaint();
    }
}
