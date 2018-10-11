#include <core.hpp>
#include <view.hpp>
#include <plugin.hpp>
#include <output.hpp>
#include "view-transform.hpp"
#include "workspace-manager.hpp"
#include "render-manager.hpp"
#include <nonstd/make_unique.hpp>

static const char* vertex_shader =
R"(
#version 100

attribute mediump vec2 position;

void main() {

    gl_Position = vec4(position.xy, 0.0, 1.0);
}
)";

static const char* up_sample_fragment_shader =
R"(
#version 100
precision mediump float;

uniform vec2 size;
uniform vec2 offset;
uniform vec2 halfpixel;
uniform sampler2D texture;

void main()
{
    vec2 uv = vec2(gl_FragCoord.xy / size);

    vec4 sum = texture2D(texture, uv + vec2(-halfpixel.x * 2.0, 0.0) * offset);
    sum += texture2D(texture, uv + vec2(-halfpixel.x, halfpixel.y) * offset) * 2.0;
    sum += texture2D(texture, uv + vec2(0.0, halfpixel.y * 2.0) * offset);
    sum += texture2D(texture, uv + vec2(halfpixel.x, halfpixel.y) * offset) * 2.0;
    sum += texture2D(texture, uv + vec2(halfpixel.x * 2.0, 0.0) * offset);
    sum += texture2D(texture, uv + vec2(halfpixel.x, -halfpixel.y) * offset) * 2.0;
    sum += texture2D(texture, uv + vec2(0.0, -halfpixel.y * 2.0) * offset);
    sum += texture2D(texture, uv + vec2(-halfpixel.x, -halfpixel.y) * offset) * 2.0;

    gl_FragColor = sum / 12.0;
}
)";

static const char* down_sample_fragment_shader =
R"(
#version 100
precision mediump float;

uniform vec2 size;
uniform vec2 offset;
uniform vec2 halfpixel;
uniform sampler2D texture;

void main()
{
    vec2 uv = vec2(gl_FragCoord.xy / size);

    vec4 sum = texture2D(texture, uv) * 4.0;
    sum += texture2D(texture, uv - halfpixel.xy * offset);
    sum += texture2D(texture, uv + halfpixel.xy * offset);
    sum += texture2D(texture, uv + vec2(halfpixel.x, -halfpixel.y) * offset);
    sum += texture2D(texture, uv - vec2(halfpixel.x, -halfpixel.y) * offset);

    gl_FragColor = sum / 8.0;
}
)";

static const char* blend_fragment_shader =
R"(
#version 100
precision mediump float;

uniform vec2 size;
uniform sampler2D window_texture;
uniform sampler2D blurred_texture;
uniform sampler2D unblurred_texture;

void main()
{
    vec4 wp = texture2D(window_texture, vec2(gl_FragCoord.xy / size));
    vec4 bp = texture2D(blurred_texture, vec2(gl_FragCoord.xy / size));
    vec4 up = texture2D(unblurred_texture, vec2(gl_FragCoord.xy / size));
    vec4 c = clamp(4.0 * wp.a, 0.0, 1.0) * bp + (1.0 - clamp(4.0 * wp.a, 0.0, 1.0)) * up;
    gl_FragColor = wp + (1.0 - wp.a) * c;
}
)";

GLuint up_prog, down_prog, blend_prog, posID, texID, sizeID, offsetID, halfpixelID;

static void
render_to_fbo(GLuint in_tex_id, GLuint out_tex_id, GLuint fbo, int width, int height)
{
	GL_CALL(glBindTexture(GL_TEXTURE_2D, out_tex_id));
	GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, 0));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
	GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
	GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out_tex_id, 0));
	GL_CALL(glActiveTexture(GL_TEXTURE0));
	GL_CALL(glBindTexture(GL_TEXTURE_2D, in_tex_id));
	GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
}

class wf_kawase_blur : public wf_view_transformer_t
{
    public:
        // the first two functions are for input transform, blur doesn't need it
        virtual wf_point local_to_transformed_point(wf_geometry view,
            wf_point point)
        {
            return point;
        }

        virtual wf_point transformed_to_local_point(wf_geometry view,
            wf_point point)
        {
            return point;
        }

        // again, nothing to transform when blurring
        virtual wlr_box get_bounding_box(wf_geometry view, wlr_box region)
        {
            return region;
        }

        uint32_t get_z_order() { return 1e9; }

        /* src_tex        the internal FBO texture,
         *
         * src_box        box of the view that has to be repainted, contains other transforms
         *
         * scissor_box    the subbox of the FB which the transform renderer must update,
         *                drawing outside of it will cause artifacts
         *
         * target_fb      the framebuffer the transform should render to.
         *                it can be part of the screen, so it's geometry is
         *                given in output-local coordinates */

        /* The above is copied from the docs
         * Now, the purpose of this function is to render src_tex
         *
         * The view has geometry src_box
         * It is in output-local coordinates
         * The target_fb.geometry is also in output-local coords
         *
         * You need to repaint ONLY inside scissor_box, just use it like view-3d.cpp#205
         *
         * target_fb contains some very useful values. First of all, you'd need
         * target_fb.tex (that is the texture of the screen as it has been rendered up to now)
         * You can copy this texture, or feed it to your shader, etc.
         * When you want to DRAW on it, use target_fb.bind(), that should be your last step
         *
         * as we both have 1920x1080 monitors without any transform or scaling (or I'm wrong?)
         * I suggest hardcoding it just to see if it works, and then we'll figure out what else
         * is there to be done. */

        virtual void render_with_damage(uint32_t src_tex,
                                        wlr_box src_box,
                                        wlr_box scissor_box,
                                        const wf_framebuffer& target_fb)
        {
	    GLuint fbo[100], tex[100];
	    int i, iterations = 3;
	    float offset = 4;
	    int width = target_fb.viewport_width;
	    int height = target_fb.viewport_height;

            float x = src_box.x, y = src_box.y, w = src_box.width, h = src_box.height;
            gl_geometry src_geometry = {x, y, x + w, y + h};

            static const float vertexData[] = {
                -1.0f, -1.0f,
                 1.0f, -1.0f,
                 1.0f,  1.0f,
                -1.0f,  1.0f
            };

            GL_CALL(glVertexAttribPointer(posID, 2, GL_FLOAT, GL_FALSE, 0, vertexData));
            GL_CALL(glEnableVertexAttribArray(posID));

	    for (i = 0; i < iterations; i++) {
	    	GL_CALL(glGenTextures(1, &tex[i]));
	    	GL_CALL(glGenFramebuffers(1, &fbo[i]));
	    }
            GL_CALL(glUseProgram(down_prog));
	    posID = GL_CALL(glGetAttribLocation(down_prog, "position"));
	    sizeID  = GL_CALL(glGetUniformLocation(down_prog, "size"));
	    offsetID  = GL_CALL(glGetUniformLocation(down_prog, "offset"));
	    halfpixelID  = GL_CALL(glGetUniformLocation(down_prog, "halfpixel"));
            glUniform2f(sizeID, w, h);
            glUniform2f(offsetID, offset, offset);
            glUniform2f(halfpixelID, 0.5f / w, 0.5f / h);

	GL_CALL(glBindTexture(GL_TEXTURE_2D, tex[0]));
	GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, 0));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
	GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]));
	GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[0], 0));

	GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, target_fb.fb));
        GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[0]));

        GL_CALL(glBlitFramebuffer(x, height - y - h, x + w, height - y, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR));

	for (i = 1; i < iterations; i++)
		render_to_fbo(tex[i - 1], tex[i], fbo[i], w, h);

            GL_CALL(glUseProgram(up_prog));
	    posID = GL_CALL(glGetAttribLocation(up_prog, "position"));
	    sizeID  = GL_CALL(glGetUniformLocation(up_prog, "size"));
	    offsetID  = GL_CALL(glGetUniformLocation(up_prog, "offset"));
	    halfpixelID  = GL_CALL(glGetUniformLocation(up_prog, "halfpixel"));
            glUniform2f(sizeID, w, h);
            glUniform2f(offsetID, offset, offset);
            glUniform2f(halfpixelID, 0.5f / w, 0.5f / h);

	for (i = iterations - 1; i > 0; i--)
		render_to_fbo(tex[i], tex[i - 1], fbo[i], w, h);

	GL_CALL(glBindTexture(GL_TEXTURE_2D, tex[3]));
	GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, 0));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
	GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, fbo[3]));
	GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[3], 0));

	GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, target_fb.fb));
        GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[3]));

        GL_CALL(glBlitFramebuffer(x, height - y - h, x + w, height - y, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR));

            GL_CALL(glUseProgram(blend_prog));

	GL_CALL(glBindTexture(GL_TEXTURE_2D, tex[1]));
	GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
	GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
	GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]));
	GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0));
	texID = GL_CALL(glGetUniformLocation(blend_prog, "window_texture"));
	glUniform1i(texID, 0);
	texID = GL_CALL(glGetUniformLocation(blend_prog, "blurred_texture"));
	glUniform1i(texID, 1);
	texID = GL_CALL(glGetUniformLocation(blend_prog, "unblurred_texture"));
	glUniform1i(texID, 2);
	sizeID  = GL_CALL(glGetUniformLocation(blend_prog, "size"));
	glUniform2f(sizeID, w, h);
            GL_CALL(glActiveTexture(GL_TEXTURE0 + 0));
            GL_CALL(glBindTexture(GL_TEXTURE_2D, src_tex));
            GL_CALL(glActiveTexture(GL_TEXTURE0 + 1));
            GL_CALL(glBindTexture(GL_TEXTURE_2D, tex[2]));
            GL_CALL(glActiveTexture(GL_TEXTURE0 + 2));
            GL_CALL(glBindTexture(GL_TEXTURE_2D, tex[3]));
            GL_CALL(glEnable(GL_BLEND));
            GL_CALL(glClearColor(0.0, 0.0, 0.0, 0.0));
            GL_CALL(glClear(GL_COLOR_BUFFER_BIT));
	GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
            GL_CALL(glActiveTexture(GL_TEXTURE0 + 0));
            // we are ready to draw to target_fb
            target_fb.bind();

            // setup scissor
            target_fb.scissor(scissor_box);

            // transform, relative to the target_fb.geometry
            auto ortho = glm::ortho(1.0f * target_fb.geometry.x, 1.0f * target_fb.geometry.x + 1.0f * target_fb.geometry.width,
                1.0f * target_fb.geometry.y + 1.0f * target_fb.geometry.height, 1.0f * target_fb.geometry.y);

            OpenGL::use_default_program();

            OpenGL::render_transformed_texture(tex[1], src_geometry, {},
                target_fb.transform * ortho);

            GL_CALL(glDisableVertexAttribArray(posID));
            GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));

	    for (i = 0; i < iterations; i++) {
	    	GL_CALL(glDeleteTextures(1, &tex[i]));
	    	GL_CALL(glDeleteFramebuffers(1, &fbo[i]));
	    }
        }

        virtual ~wf_kawase_blur() {}
};

class wayfire_kawase : public wayfire_plugin_t
{
    button_callback btn;
    effect_hook_t damage;
    post_hook_t passthrough;

    public:
    void init(wayfire_config *config)
    {
        grab_interface->name = "kawase";
        grab_interface->abilities_mask = WF_ABILITY_CONTROL_WM;

        btn = [=] (uint32_t, int, int)
        {

            auto focus = core->get_cursor_focus();

            if (!focus)
                return;

            auto view = core->find_view(focus->get_main_surface());
            view->add_transformer(nonstd::make_unique<wf_kawase_blur> ());

            output->render->add_post(&passthrough);
        };

        output->add_button(new_static_option("<super> <alt> BTN_LEFT"), &btn);

        damage = [=] () {
            output->render->damage(NULL);
        };

        output->render->add_effect(&damage, WF_OUTPUT_EFFECT_PRE);

        passthrough = [=] (uint32_t fb, uint32_t tex, uint32_t target)
        {
            auto w = output->handle->width;
            auto h = output->handle->height;

            GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, fb));
            GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target));

            GL_CALL(glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR));
            GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        };

        auto vs = OpenGL::compile_shader(vertex_shader, GL_VERTEX_SHADER);
        auto ufs = OpenGL::compile_shader(up_sample_fragment_shader, GL_FRAGMENT_SHADER);
        auto dfs = OpenGL::compile_shader(down_sample_fragment_shader, GL_FRAGMENT_SHADER);
        auto bfs = OpenGL::compile_shader(blend_fragment_shader, GL_FRAGMENT_SHADER);

        up_prog = GL_CALL(glCreateProgram());
        GL_CALL(glAttachShader(up_prog, vs));
        GL_CALL(glAttachShader(up_prog, ufs));
        GL_CALL(glLinkProgram(up_prog));

        down_prog = GL_CALL(glCreateProgram());
        GL_CALL(glAttachShader(down_prog, vs));
        GL_CALL(glAttachShader(down_prog, dfs));
        GL_CALL(glLinkProgram(down_prog));

        blend_prog = GL_CALL(glCreateProgram());
        GL_CALL(glAttachShader(blend_prog, vs));
        GL_CALL(glAttachShader(blend_prog, bfs));
        GL_CALL(glLinkProgram(blend_prog));

        /* won't be really deleted until program is deleted as well */
        GL_CALL(glDeleteShader(vs));
        GL_CALL(glDeleteShader(ufs));
        GL_CALL(glDeleteShader(dfs));
        GL_CALL(glDeleteShader(bfs));
    }

    void fini()
    {
        GL_CALL(glDeleteProgram(up_prog));
        GL_CALL(glDeleteProgram(down_prog));
        GL_CALL(glDeleteProgram(blend_prog));
    }
};

extern "C"
{
    wayfire_plugin_t *newInstance()
    {
        return new wayfire_kawase();
    }
}
