#ifndef VIEW_TRANSFORM_HPP
#define VIEW_TRANSFORM_HPP

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "view.hpp"
#include "opengl.hpp"
#include "debug.hpp"

enum wf_transformer_z_order
{
    WF_TRANSFORMER_2D = 1,
    WF_TRANSFORMER_3D = 2,
    WF_TRANSFORMER_HIGHLEVEL = 500,
};

class wf_view_transformer_t
{
    public:
        /* return how this transform should be placed with respect to other transforms
         * Higher numbers indicate that this transform should come after other transforms */
        virtual uint32_t get_z_order() = 0;

        virtual wf_point local_to_transformed_point(wf_geometry view, wf_point point) = 0;
        virtual wf_point transformed_to_local_point(wf_geometry view, wf_point point) = 0;

        /* return the boundingbox of region after applying all transformations */
        virtual wlr_box get_bounding_box(wf_geometry view, wlr_box region);

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
        virtual void render_with_damage(uint32_t src_tex,
                                        wlr_box src_box,
                                        wlr_box scissor_box,
                                        const wf_framebuffer& target_fb) = 0;

        virtual ~wf_view_transformer_t() {log_info("destoryed"); }
};

enum
{
    WF_INVALID_INPUT_COORDINATES = (1 << 31)
};

/* 2D transforms operate with a coordinate system centered at the
 * center of the main surface(the wayfire_view_t) */
class wf_2D_view : public wf_view_transformer_t
{
    protected:
        wayfire_view view;
    public:
        float angle = 0.0f;
        float scale_x = 1.0f, scale_y = 1.0f;
        float translation_x = 0.0f, translation_y = 0.0f;
        float alpha = 1.0f;

    public:
        wf_2D_view(wayfire_view view);

        virtual uint32_t get_z_order() { return WF_TRANSFORMER_2D; }

        virtual wf_point local_to_transformed_point(wf_geometry view, wf_point point);
        virtual wf_point transformed_to_local_point(wf_geometry view, wf_point point);

        virtual void render_with_damage(uint32_t src_tex,
                                        wlr_box src_box,
                                        wlr_box scissor_box,
                                        const wf_framebuffer& target_fb);
};

/* Those are centered relative to the view's bounding box */
class wf_3D_view : public wf_view_transformer_t
{
    protected:
        wayfire_view view;

    public:
        glm::mat4 view_proj{1.0}, translation{1.0}, rotation{1.0}, scaling{1.0};
        glm::vec4 color{1, 1, 1, 1};

        glm::mat4 calculate_total_transform();

    public:
        wf_3D_view(wayfire_view view);

        virtual uint32_t get_z_order() { return WF_TRANSFORMER_3D; }

        virtual wf_point local_to_transformed_point(wf_geometry view, wf_point point);
        virtual wf_point transformed_to_local_point(wf_geometry view, wf_point point);

        virtual void render_with_damage(uint32_t src_tex,
                                        wlr_box src_box,
                                        wlr_box scissor_box,
                                        const wf_framebuffer& target_fb);

        static const float fov; // PI / 8
        static glm::mat4 default_view_matrix();
        static glm::mat4 default_proj_matrix();
};

/* create a matrix which corresponds to the inverse of the given transform */
glm::mat4 get_output_matrix_from_transform(wl_output_transform transform);

/* a matrix which can be used to render wf_geometry directly */
glm::mat4 output_get_projection(wayfire_output *output);

#endif /* end of include guard: VIEW_TRANSFORM_HPP */
