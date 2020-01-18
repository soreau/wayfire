#include "blur.hpp"
#include <wayfire/debug.hpp>
#include <wayfire/output.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/util/log.hpp>

static const char* blur_blend_vertex_shader = R"(
#version 100

attribute mediump vec2 position;
varying mediump vec2 uvpos[2];

uniform mat4 mvp;

void main() {

    gl_Position = vec4(position.xy, 0.0, 1.0);
    uvpos[0] = (position.xy + vec2(1.0, 1.0)) / 2.0;
    uvpos[1] = vec4(mvp * vec4(uvpos[0] - 0.5, 0.0, 1.0)).xy + 0.5;
})";

static const char* blur_blend_fragment_shader = R"(
#version 100
@builtin_ext@
precision mediump float;

@builtin@
uniform sampler2D bg_texture;

varying mediump vec2 uvpos[2];

void main()
{
    vec4 bp = texture2D(bg_texture, uvpos[0]);
    vec4 wp = get_pixel(uvpos[1]);
    vec4 c = clamp(4.0 * wp.a, 0.0, 1.0) * bp;
    gl_FragColor = wp + (1.0 - wp.a) * c;
})";

wf_blur_base::wf_blur_base(wf::output_t *output,
    const wf_blur_default_option_values& defaults)
{
    this->output = output;
    this->algorithm_name = defaults.algorithm_name;

    this->offset_opt.load_option("blur/" + algorithm_name + "_offset");
    this->degrade_opt.load_option("blur/" + algorithm_name + "_degrade");
    this->iterations_opt.load_option("blur/" + algorithm_name + "_iterations");

    this->options_changed = [=] () { damage_all_workspaces(); };
    this->offset_opt.set_callback(options_changed);
    this->degrade_opt.set_callback(options_changed);
    this->iterations_opt.set_callback(options_changed);

    OpenGL::render_begin();
    blend_program.compile(blur_blend_vertex_shader, blur_blend_fragment_shader);
    OpenGL::render_end();
}

wf_blur_base::~wf_blur_base()
{
    OpenGL::render_begin();
    fb[0].release();
    fb[1].release();
    program[0].free_resources();
    program[1].free_resources();
    blend_program.free_resources();
    OpenGL::render_end();
}

int wf_blur_base::calculate_blur_radius()
{
    return offset_opt * degrade_opt * iterations_opt;
}

void wf_blur_base::damage_all_workspaces()
{
    auto wsize = output->workspace->get_workspace_grid_size();
    for (int vx = 0; vx < wsize.width; vx++)
    {
        for (int vy = 0; vy < wsize.height; vy++)
        {
            output->render->damage(
                output->render->get_ws_box({vx, vy}));
        }
    }
}

void wf_blur_base::render_iteration(wf::framebuffer_base_t& in,
    wf::framebuffer_base_t& out, int width, int height)
{
    /* Special case for small regions where we can't really blur, because we
     * simply have too few pixels */
    width = std::max(width, 1);
    height = std::max(height, 1);

    out.allocate(width, height);
    out.bind();

    GL_CALL(glBindTexture(GL_TEXTURE_2D, in.tex));
    GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
}

wlr_box wf_blur_base::copy_region(wf::framebuffer_base_t& result,
    const wf::framebuffer_t& source, const wf::region_t& region)
{
    auto subbox = source.framebuffer_box_from_damage_box(
        wlr_box_from_pixman_box(region.get_extents()));

    auto source_box = source.framebuffer_box_from_geometry_box({
        0, 0, source.geometry.width, source.geometry.height});

    /* Scaling down might cause issues like flickering or some discrepancies
     * between the source and final image.
     * To make things a bit more stable, we first blit to a size which
     * is divisble by degrade */
    int degrade = degrade_opt;
    int rounded_width = std::max(1, subbox.width + subbox.width % degrade);
    int rounded_height = std::max(1, subbox.height + subbox.height % degrade);

    OpenGL::render_begin(source);
    result.allocate(rounded_width, rounded_height);

    GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, source.fb));
    GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, result.fb));
    GL_CALL(glBlitFramebuffer(
            subbox.x, source_box.height - subbox.y - subbox.height,
            subbox.x + subbox.width, source_box.height - subbox.y,
            0, 0, rounded_width, rounded_height,
            GL_COLOR_BUFFER_BIT, GL_LINEAR));
    OpenGL::render_end();

    return subbox;
}

void wf_blur_base::pre_render(wf::texture_t src_tex, wlr_box src_box,
    const wf::region_t& damage, const wf::framebuffer_t& target_fb)
{
    int degrade = degrade_opt;
    auto damage_box = copy_region(fb[0], target_fb, damage);
    int scaled_width = std::max(1, damage_box.width / degrade);
    int scaled_height = std::max(1, damage_box.height / degrade);

    int r = blur_fb0(scaled_width, scaled_height);

    /* Make sure the result is always fb[1], because that's what is used in render() */
    if (r != 0)
        std::swap(fb[0], fb[1]);

    /* Support iterations = 0 */
    if (iterations_opt == 0 && algorithm_name != "bokeh")
    {
        int rounded_width = std::max(1, damage_box.width + damage_box.width % degrade);
        int rounded_height = std::max(1, damage_box.height + damage_box.height % degrade);
        OpenGL::render_begin();
        fb[1].allocate(scaled_width, scaled_height);
        fb[1].bind();
        GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0].fb));
        GL_CALL(glBlitFramebuffer(0, 0, rounded_width, rounded_height,
                0, 0, scaled_width, scaled_height,
                GL_COLOR_BUFFER_BIT, GL_LINEAR));
        OpenGL::render_end();
        std::swap(fb[0], fb[1]);
    }

    /* we subtract target_fb's position to so that
     * view box is relative to framebuffer */
    auto view_box = target_fb.framebuffer_box_from_geometry_box(
        src_box + wf::point_t{-target_fb.geometry.x, -target_fb.geometry.y});

    OpenGL::render_begin();
    fb[1].allocate(view_box.width, view_box.height);
    fb[1].bind();
    GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0].fb));

    /* Blit the blurred texture into an fb which has the size of the view,
     * so that the view texture and the blurred background can be combined
     * together in render()
     *
     * local_geometry is damage_box relative to view box */
    wlr_box local_box = damage_box + wf::point_t{-view_box.x, -view_box.y};
    GL_CALL(glBlitFramebuffer(0, 0, scaled_width, scaled_height,
            local_box.x,
            view_box.height - local_box.y - local_box.height,
            local_box.x + local_box.width,
            view_box.height - local_box.y,
            GL_COLOR_BUFFER_BIT, GL_LINEAR));
    GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
    OpenGL::render_end();
}

void wf_blur_base::render(wf::texture_t src_tex, wlr_box src_box,
    wlr_box scissor_box, const wf::framebuffer_t& target_fb)
{
    wlr_box fb_geom = target_fb.framebuffer_box_from_geometry_box(target_fb.geometry);
    auto view_box = target_fb.framebuffer_box_from_geometry_box(src_box);
    view_box.x -= fb_geom.x;
    view_box.y -= fb_geom.y;

    OpenGL::render_begin(target_fb);
    blend_program.use(src_tex.type);

    /* Use shader and enable vertex and texcoord data */
    static const float vertexData[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f
    };

    blend_program.attrib_pointer("position", 2, 0, vertexData);

    /* Blend blurred background with window texture src_tex */
    blend_program.uniformMatrix4f("mvp", glm::inverse(target_fb.transform));
    /* XXX: core should give us the number of texture units used */
    blend_program.uniform1i("bg_texture", 1);

    blend_program.set_active_texture(src_tex);
    GL_CALL(glActiveTexture(GL_TEXTURE0 + 1));
    GL_CALL(glBindTexture(GL_TEXTURE_2D, fb[1].tex));
    /* Render it to target_fb */
    target_fb.bind();
    GL_CALL(glViewport(view_box.x, fb_geom.height - view_box.y - view_box.height,
            view_box.width, view_box.height));
    target_fb.scissor(scissor_box);

    GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));

    /* Disable stuff */
    /* GL_CALL(glActiveTexture(GL_TEXTURE0 + 1)); */
    GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
    GL_CALL(glActiveTexture(GL_TEXTURE0));
    GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
    blend_program.deactivate();
    OpenGL::render_end();
}

std::unique_ptr<wf_blur_base> create_blur_from_name(wf::output_t *output,
    std::string algorithm_name)
{
    if (algorithm_name == "box")
        return create_box_blur(output);
    if (algorithm_name == "bokeh")
        return create_bokeh_blur(output);
    if (algorithm_name == "kawase")
        return create_kawase_blur(output);
    if (algorithm_name == "gaussian")
        return create_gaussian_blur(output);

    LOGE ("Unrecognized blur algorithm %s. Using default kawase blur.",
        algorithm_name.c_str());
    return create_kawase_blur(output);
}
