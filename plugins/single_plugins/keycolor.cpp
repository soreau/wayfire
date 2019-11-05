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

#include <core.hpp>
#include <view.hpp>
#include <plugin.hpp>
#include <output.hpp>
#include <signal-definitions.hpp>
#include "view-transform.hpp"
#include "workspace-manager.hpp"


static const char* vertex_shader =
R"(
#version 100

attribute mediump vec2 position;
attribute mediump vec2 texcoord;

varying mediump vec2 uvpos;

void main() {

   gl_Position = vec4(position.xy, 0.0, 1.0);
   uvpos = texcoord;
}
)";

static const char* fragment_shader =
R"(
#version 100
precision mediump float;

uniform sampler2D window_texture;
uniform float color[4];
uniform float threshold;

varying mediump vec2 uvpos;

void main()
{
    vec2 uv = uvpos;
    vec4 c = texture2D(window_texture, uv);
    vec4 vdiff = abs(vec4(color[0], color[1], color[2], 1.0) - c);
    float diff = max(max(max(vdiff.r, vdiff.g), vdiff.b), vdiff.a);
    if (diff < threshold) {
        c  *= color[3];
        c.a = color[3];
    }
    gl_FragColor = c;
}
)";

static GLuint prog, pos_id, tex_id, color_id, threshold_id, texcoord_id;
wf_option color_opt, opacity_opt, threshold_opt;

class wf_keycolor : public wf_view_transformer_t
{
    public:
        virtual wf_pointf local_to_transformed_point(wf_geometry view,
            wf_pointf point)
        {
            return point;
        }

        virtual wf_pointf transformed_to_local_point(wf_geometry view,
            wf_pointf point)
        {
            return point;
        }

        virtual wlr_box get_bounding_box(wf_geometry view, wlr_box region)
        {
            return region;
        }

        uint32_t get_z_order() { return WF_TRANSFORMER_2D; }

        virtual void render_with_damage(uint32_t src_tex,
                                        wlr_box src_box,
                                        wlr_box scissor_box,
                                        const wf_framebuffer& target_fb)
        {
        }

        virtual void render_box(uint32_t src_tex,
                                wlr_box _src_box,
                                wlr_box scissor_box,
                                const wf_framebuffer& target_fb)
        {
            auto src_box = _src_box;
            int fb_h = target_fb.viewport_height;

            src_box.x -= target_fb.geometry.x;
            src_box.y -= target_fb.geometry.y;

            float x = src_box.x, y = src_box.y, w = src_box.width, h = src_box.height;

            static const float vertexData[] = {
                -1.0f, -1.0f,
                 1.0f, -1.0f,
                 1.0f,  1.0f,
                -1.0f,  1.0f
            };
            static const float texCoords[] = {
                 0.0f, 0.0f,
                 1.0f, 0.0f,
                 1.0f, 1.0f,
                 0.0f, 1.0f
            };

            OpenGL::render_begin(target_fb);

            /* Upload data to shader */
            GLfloat color[4] = {color_opt->as_color().r, color_opt->as_color().g, color_opt->as_color().b, (GLfloat) opacity_opt->as_double()};
            GLfloat threshold = threshold_opt->as_double();
            GL_CALL(glUseProgram(prog));
            GL_CALL(glUniform1fv(color_id, 4, color));
            GL_CALL(glUniform1f(threshold_id, threshold));
            GL_CALL(glVertexAttribPointer(pos_id, 2, GL_FLOAT, GL_FALSE, 0, vertexData));
            GL_CALL(glVertexAttribPointer(texcoord_id, 2, GL_FLOAT, GL_FALSE, 0, texCoords));
            GL_CALL(glEnableVertexAttribArray(pos_id));
            GL_CALL(glEnableVertexAttribArray(texcoord_id));
            GL_CALL(glUniform1i(tex_id, 0));
            GL_CALL(glActiveTexture(GL_TEXTURE0 + 0));
            GL_CALL(glBindTexture(GL_TEXTURE_2D, src_tex));

            /* Render it to target_fb */
            target_fb.bind();

            GL_CALL(glViewport(x, fb_h - y - h, w, h));
            target_fb.scissor(scissor_box);
            GL_CALL(glEnable(GL_BLEND));
            GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));

            GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));

            /* Disable stuff */
            GL_CALL(glDisable(GL_BLEND));
            GL_CALL(glActiveTexture(GL_TEXTURE0));
            GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
            GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
            GL_CALL(glDisableVertexAttribArray(pos_id));
            GL_CALL(glDisableVertexAttribArray(texcoord_id));

            OpenGL::render_end();
        }

        virtual ~wf_keycolor() {}
};

class wayfire_keycolor : public wf::plugin_interface_t
{
    const std::string transformer_name = "keycolor";
    wf::signal_callback_t view_attached, view_detached;
    wf_option_callback option_changed;

    void add_transformer(wayfire_view view)
    {
        if (view->get_transformer(transformer_name))
            return;

        view->add_transformer(std::make_unique<wf_keycolor> (),
            transformer_name);
    }

    void pop_transformer(wayfire_view view)
    {
        if (view->get_transformer(transformer_name))
            view->pop_transformer(transformer_name);
    }

    void remove_transformers()
    {
        for (auto& view : output->workspace->get_views_in_layer(wf::ALL_LAYERS))
            if (view->get_transformer(transformer_name))
                pop_transformer(view);
    }

    public:
    void init(wayfire_config *config)
    {
        grab_interface->name = transformer_name;
        grab_interface->capabilities = 0;

        auto section = config->get_section("keycolor");
        color_opt = section->get_option("color", "0 0 0 1");
        opacity_opt = section->get_option("opacity", "0.25");
        threshold_opt = section->get_option("threshold", "0.5");

        option_changed = [=] ()
        {
            for (auto& view : output->workspace->get_views_in_layer(wf::ALL_LAYERS))
            {
                if (view->get_transformer(transformer_name))
                     view->damage();
            }
        };

        color_opt->add_updated_handler(&option_changed);
        opacity_opt->add_updated_handler(&option_changed);
        threshold_opt->add_updated_handler(&option_changed);

        view_attached = [=] (wf::signal_data_t *data)
        {
            auto view = get_signaled_view(data);
            if (view->role == wf::VIEW_ROLE_SHELL_VIEW)
                return;
            assert(!view->get_transformer(transformer_name));
            add_transformer(view);
        };

        /* If a view is detached, we remove its blur transformer.
         * If it is just moved to another output, the blur plugin
         * on the other output will add its own transformer there */
        view_detached = [=] (wf::signal_data_t *data)
        {
            auto view = get_signaled_view(data);
            pop_transformer(view);
        };
        output->connect_signal("attach-view", &view_attached);
        output->connect_signal("detach-view", &view_detached);

        OpenGL::render_begin();
        auto vs = OpenGL::compile_shader(vertex_shader, GL_VERTEX_SHADER);
        auto fs = OpenGL::compile_shader(fragment_shader, GL_FRAGMENT_SHADER);

        prog = GL_CALL(glCreateProgram());
        GL_CALL(glAttachShader(prog, vs));
        GL_CALL(glAttachShader(prog, fs));
        GL_CALL(glLinkProgram(prog));

        GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        pos_id = GL_CALL(glGetAttribLocation(prog, "position"));
        texcoord_id = GL_CALL(glGetAttribLocation(prog, "texcoord"));
        tex_id = GL_CALL(glGetUniformLocation(prog, "window_texture"));
        color_id = GL_CALL(glGetUniformLocation(prog, "color"));
        threshold_id = GL_CALL(glGetUniformLocation(prog, "threshold"));

        /* won't be really deleted until program is deleted as well */
        GL_CALL(glDeleteShader(vs));
        GL_CALL(glDeleteShader(fs));
        OpenGL::render_end();

        for (auto& view : output->workspace->get_views_in_layer(wf::ALL_LAYERS))
        {
            if (view->role == wf::VIEW_ROLE_SHELL_VIEW)
                continue;
            add_transformer(view);
        }
    }

    void fini()
    {
        remove_transformers();
        output->disconnect_signal("attach-view", &view_attached);
        output->disconnect_signal("detach-view", &view_detached);
        color_opt->rem_updated_handler(&option_changed);
        opacity_opt->rem_updated_handler(&option_changed);
        threshold_opt->rem_updated_handler(&option_changed);
        GL_CALL(glDeleteProgram(prog));
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_keycolor);
