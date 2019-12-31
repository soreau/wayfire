#ifndef VIEW_TRANSFORM_HPP
#define VIEW_TRANSFORM_HPP

#include <wayfire/view.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/debug.hpp>

namespace wf
{
enum transformer_z_order_t
{
    /* Simple 2D transforms */
    TRANSFORMER_2D = 1,
    /* 3D transforms */
    TRANSFORMER_3D = 2,
    /* Highlevels transforms and above do special effects, for ex. wobbly or fire */
    TRANSFORMER_HIGHLEVEL = 500,
    /* Do not use Z oder blur or more,
     * except if you are willing to break it */
    TRANSFORMER_BLUR = 999,
};

class view_transformer_t
{
    public:
        /* return how this transform should be placed with respect to other transforms
         * Higher numbers indicate that this transform should come after other transforms */
        virtual uint32_t get_z_order() = 0;

        // TODO: rename
        // transformed_to_local_point -> untransform/reverse()
        virtual wf::pointf_t local_to_transformed_point(wf::geometry_t view, wf::pointf_t point) = 0;
        virtual wf::pointf_t transformed_to_local_point(wf::geometry_t view, wf::pointf_t point) = 0;

        /* return the boundingbox of region after applying all transformations */
        virtual wlr_box get_bounding_box(wf::geometry_t view, wlr_box region);

        /* src_tex        the internal FBO texture,
         *
         * src_box        box of the view that has to be repainted, contains
         *                other transforms
         *
         * damage         the region that needs to be repainted, clipped to
         *                the view's geometry
         *
         * target_fb      the framebuffer the transform should render to.
         *                it can be part of the screen, so it's geometry is
         *                given in output-local coordinates
         *
         * The default implementation of render_with_damage() will simply iterate
         * over all rectangles in the damage region, apply framebuffer transform
         * to it and then call render_box(). Plugins can override either of the
         * functions.
         * */
        virtual void render_with_damage(wf::texture_t src_tex, wlr_box src_box,
            const wf::region_t& damage, const wf::framebuffer_t& target_fb);

        virtual void render_box(wf::texture_t src_tex, wlr_box src_box,
            wlr_box scissor_box, const wf::framebuffer_t& target_fb) {}

        virtual ~view_transformer_t() {}
};

/* 2D transforms operate with a coordinate system centered at the
 * center of the main surface(the wayfire_view_t) */
class view_2D : public view_transformer_t
{
    protected:
        wayfire_view view;
    public:
        float angle = 0.0f;
        float scale_x = 1.0f, scale_y = 1.0f;
        float translation_x = 0.0f, translation_y = 0.0f;
        float alpha = 1.0f;

    public:
        view_2D(wayfire_view view);

        virtual uint32_t get_z_order() { return TRANSFORMER_2D; }

        virtual wf::pointf_t local_to_transformed_point(wf::geometry_t view, wf::pointf_t point);
        virtual wf::pointf_t transformed_to_local_point(wf::geometry_t view, wf::pointf_t point);

        virtual void render_box(wf::texture_t src_tex, wlr_box src_box,
            wlr_box scissor_box, const wf::framebuffer_t& target_fb);
};

/* Those are centered relative to the view's bounding box */
class view_3D : public view_transformer_t
{
    protected:
        wayfire_view view;

    public:
        glm::mat4 view_proj{1.0}, translation{1.0}, rotation{1.0}, scaling{1.0};
        glm::vec4 color{1, 1, 1, 1};

        glm::mat4 calculate_total_transform();

    public:
        view_3D(wayfire_view view);

        virtual uint32_t get_z_order() { return TRANSFORMER_3D; }

        virtual wf::pointf_t local_to_transformed_point(wf::geometry_t view, wf::pointf_t point);
        virtual wf::pointf_t transformed_to_local_point(wf::geometry_t view, wf::pointf_t point);

        virtual void render_box(wf::texture_t src_tex, wlr_box src_box,
            wlr_box scissor_box, const wf::framebuffer_t& target_fb);

        static const float fov; // PI / 8
        static glm::mat4 default_view_matrix();
        static glm::mat4 default_proj_matrix();
};

/* create a matrix which corresponds to the inverse of the given transform */
glm::mat4 get_output_matrix_from_transform(wl_output_transform transform);

/* a matrix which can be used to render wf::geometry_t directly */
glm::mat4 output_get_projection(wf::output_t *output);
}

#endif /* end of include guard: VIEW_TRANSFORM_HPP */
