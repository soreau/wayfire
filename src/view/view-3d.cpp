#include "wayfire/view-transform.hpp"
#include "wayfire/opengl.hpp"
#include "wayfire/debug.hpp"
#include "wayfire/core.hpp"
#include "wayfire/output.hpp"
#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#define PI 3.14159265359

wlr_box wf::view_transformer_t::get_bounding_box(wf::geometry_t view, wlr_box region)
{
    const auto p1 = transform_point(view, {1.0 * region.x,                1.0 * region.y});
    const auto p2 = transform_point(view, {1.0 * region.x + region.width, 1.0 * region.y});
    const auto p3 = transform_point(view, {1.0 * region.x,                1.0 * region.y + region.height});
    const auto p4 = transform_point(view, {1.0 * region.x + region.width, 1.0 * region.y + region.height});

    const int x1 = std::min({p1.x, p2.x, p3.x, p4.x});
    const int x2 = std::max({p1.x, p2.x, p3.x, p4.x});
    const int y1 = std::min({p1.y, p2.y, p3.y, p4.y});
    const int y2 = std::max({p1.y, p2.y, p3.y, p4.y});

    return wlr_box{x1, y1, x2 - x1, y2 - y1};
}

wf::region_t wf::view_transformer_t::transform_opaque_region(
    wf::geometry_t box, wf::region_t region)
{
    return {};
}

void wf::view_transformer_t::render_with_damage(wf::texture_t src_tex, wlr_box src_box,
            const wf::region_t& damage, const wf::framebuffer_t& target_fb)
{
    for (const auto& rect : damage)
    {
        auto box = target_fb.framebuffer_box_from_damage_box(
            wlr_box_from_pixman_box(rect));
        render_box(src_tex, src_box, box, target_fb);
    }
}

struct transformable_quad
{
    gl_geometry geometry;
    float off_x, off_y;
};

static wf::point_t get_center(wf::geometry_t view)
{
    return {
        view.x + view.width / 2,
        view.y + view.height/ 2
    };
}

static wf::pointf_t get_center_relative_coords(wf::geometry_t view, wf::pointf_t point)
{
    return {
        (point.x - view.x) - view.width / 2.0,
        view.height / 2.0 - (point.y - view.y)
    };
}

static wf::pointf_t get_absolute_coords_from_relative(wf::geometry_t view, wf::pointf_t point)
{
    return {
        point.x + view.x + view.width / 2.0,
        (view.height / 2.0 - point.y) + view.y
    };
}

static transformable_quad center_geometry(wf::geometry_t output_geometry,
                                          wf::geometry_t geometry,
                                          wf::point_t target_center)
{
    transformable_quad quad;

    geometry.x -= output_geometry.x;
    geometry.y -= output_geometry.y;

    target_center.x -= output_geometry.x;
    target_center.y -= output_geometry.y;

    quad.geometry.x1 = -(target_center.x - geometry.x);
    quad.geometry.y1 =  (target_center.y - geometry.y);

    quad.geometry.x2 = quad.geometry.x1 + geometry.width;
    quad.geometry.y2 = quad.geometry.y1 - geometry.height;

    quad.off_x = (geometry.x - output_geometry.width / 2.0)  - quad.geometry.x1;
    quad.off_y = (output_geometry.height / 2.0 - geometry.y) - quad.geometry.y1;

    return quad;
}

wf::view_2D::view_2D(wayfire_view view)
{
    this->view = view;
}

static void rotate_xy(float& x, float& y, float angle)
{
    auto v = glm::vec4{x, y, 0, 1};
    auto rot = glm::rotate(glm::mat4(1.0), angle, {0, 0, 1});
    v = rot * v;
    x = v.x;
    y = v.y;
}

wf::pointf_t wf::view_2D::transform_point(
    wf::geometry_t geometry, wf::pointf_t point)
{

    auto p2 = get_center_relative_coords(view->get_wm_geometry(), point);
    float x = p2.x, y = p2.y;

    x *= scale_x; y *= scale_y;
    rotate_xy(x, y, angle);
    x += translation_x;
    y -= translation_y;

    auto r = get_absolute_coords_from_relative(view->get_wm_geometry(), {x, y});
    return r;
}

wf::pointf_t wf::view_2D::untransform_point(
    wf::geometry_t geometry, wf::pointf_t point)
{
    point = get_center_relative_coords(view->get_wm_geometry(), point);
    float x = point.x, y = point.y;

    x -= translation_x;
    y += translation_y;
    rotate_xy(x, y, -angle);
    x /= scale_x; y /= scale_y;

    return get_absolute_coords_from_relative(view->get_wm_geometry(), {x, y});
}

void wf::view_2D::render_box(wf::texture_t src_tex, wlr_box src_box,
    wlr_box scissor_box, const wf::framebuffer_t& fb)
{
    auto quad = center_geometry(fb.geometry, src_box, get_center(view->get_wm_geometry()));

    quad.geometry.x1 *= scale_x;
    quad.geometry.x2 *= scale_x;
    quad.geometry.y1 *= scale_y;
    quad.geometry.y2 *= scale_y;

    auto rotate = glm::rotate(glm::mat4(1.0), angle, {0, 0, 1});
    auto translate = glm::translate(glm::mat4(1.0),
                                    {quad.off_x + translation_x,
                                     quad.off_y - translation_y, 0});

    auto ortho = glm::ortho(-fb.geometry.width  / 2.0f, fb.geometry.width  / 2.0f,
                            -fb.geometry.height / 2.0f, fb.geometry.height / 2.0f);

    auto transform = fb.transform * ortho * translate * rotate;

    OpenGL::render_begin(fb);
    fb.scissor(scissor_box);
    OpenGL::render_transformed_texture(src_tex, quad.geometry, {},
                                       transform, {1.0f, 1.0f, 1.0f, alpha});
    OpenGL::render_end();
}

const float wf::view_3D::fov = PI/4;
glm::mat4 wf::view_3D::default_view_matrix()
{
    return glm::lookAt(
        glm::vec3(0., 0., 1.0 / std::tan(fov / 2)),
        glm::vec3(0., 0., 0.),
        glm::vec3(0., 1., 0.));
}

glm::mat4 wf::view_3D::default_proj_matrix()
{
    return glm::perspective(fov, 1.0f, .1f, 100.f);
}

wf::view_3D::view_3D(wayfire_view view)
{
    this->view = view;
    view_proj = default_proj_matrix() * default_view_matrix();
}

/* TODO: cache total_transform, because it is often unnecessarily recomputed */
glm::mat4 wf::view_3D::calculate_total_transform()
{
    auto og = view->get_output()->get_relative_geometry();
    glm::mat4 depth_scale = glm::scale(glm::mat4(1.0), {1, 1, 2.0 / std::min(og.width, og.height)});
    return translation * view_proj * depth_scale * rotation * scaling;
}

wf::pointf_t wf::view_3D::transform_point(
    wf::geometry_t geometry, wf::pointf_t point)
{
    auto p = get_center_relative_coords(geometry, point);
    glm::vec4 v(1.0f * p.x, 1.0f * p.y, 0, 1);
    v = calculate_total_transform() * v;

    v.x /= v.w;
    v.y /= v.w;

    return get_absolute_coords_from_relative(geometry, {v.x, v.y});
}

/* TODO: is there a way to realiably reverse projective transformations? */
wf::pointf_t wf::view_3D::untransform_point(wf::geometry_t geometry, wf::pointf_t point)
{
    return {wf::compositor_core_t::invalid_coordinate,
        wf::compositor_core_t::invalid_coordinate};
}

void wf::view_3D::render_box(wf::texture_t src_tex, wlr_box src_box,
    wlr_box scissor_box, const wf::framebuffer_t& fb)
{
    auto quad = center_geometry(fb.geometry, src_box, get_center(src_box));

    auto transform = calculate_total_transform();
    auto translate = glm::translate(glm::mat4(1.0), {quad.off_x, quad.off_y, 0});
    auto scale = glm::scale(glm::mat4(1.0), {
                                2.0 / fb.geometry.width,
                                2.0 / fb.geometry.height,
                                1.0
                            });

    transform = fb.transform * scale * translate * transform;

    OpenGL::render_begin(fb);
    fb.scissor(scissor_box);
    OpenGL::render_transformed_texture(src_tex, quad.geometry, {},
                                       transform, color);
    OpenGL::render_end();
}
