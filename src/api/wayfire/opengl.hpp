#ifndef WF_OPENGL_HPP
#define WF_OPENGL_HPP

#include <GLES3/gl3.h>

#include <wayfire/config/types.hpp>
#include <wayfire/util.hpp>
#include <wayfire/nonstd/noncopyable.hpp>

#include <wayfire/geometry.hpp>

#define GLM_FORCE_RADIANS
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

void gl_call(const char*, uint32_t, const char*);

#ifndef __STRING
#  define __STRING(x) #x
#endif

/* recommended to use this to make OpenGL calls, since it offers easier debugging */
/* This macro is taken from WLC source code */
#define GL_CALL(x) x; gl_call(__PRETTY_FUNCTION__, __LINE__, __STRING(x))

#define TEXTURE_TRANSFORM_INVERT_X     (1 << 0)
#define TEXTURE_TRANSFORM_INVERT_Y     (1 << 1)
#define TEXTURE_USE_TEX_GEOMETRY       (1 << 2)

struct gl_geometry
{
    float x1, y1, x2, y2;
};

namespace wf
{
/* Simple framebuffer, used mostly to allocate framebuffers for workspace
 * streams.
 *
 * Resources (tex/fb) are not automatically destroyed */
struct framebuffer_base_t : public noncopyable_t
{
    GLuint tex = -1, fb = -1;
    int32_t viewport_width = 0, viewport_height = 0;

    framebuffer_base_t() = default;
    framebuffer_base_t(framebuffer_base_t&& other);
    framebuffer_base_t& operator = (framebuffer_base_t&& other);

    /* The functions below assume they are called between
     * OpenGL::render_begin() and OpenGL::render_end() */

    /* will invalidate texture contents if width or height changes.
     * If tex and/or fb haven't been set, it creates them
     * Return true if texture was created/invalidated */
    bool allocate(int width, int height);

    /* Make the framebuffer current, and adjust viewport to its size */
    void bind() const;

    /* Set the GL scissor to the given box, after inverting it to match GL
     * coordinate space */
    void scissor(wlr_box box) const;

    /* Will destroy the texture and framebuffer
     * Warning: will destroy tex/fb even if they have been allocated outside of
     * allocate() */
    void release();

    /* Reset the framebuffer, WITHOUT freeing resources.
     * There is no need to call reset() after release() */
    void reset();

    private:
    void copy_state(framebuffer_base_t&& other);
};

/* A more feature-complete framebuffer.
 * It represents an area of the output, with the corresponding dimensions,
 * transforms, etc */
struct framebuffer_t : public framebuffer_base_t
{
    wf::geometry_t geometry = {0, 0, 0, 0};

    uint32_t wl_transform = WL_OUTPUT_TRANSFORM_NORMAL;
    float scale = 1.0;

    /* Indicates if the framebuffer has other transform than indicated
     * by scale and wl_transform */
    bool has_nonstandard_transform = false;

    /* Transform contains output rotation, and possibly
     * other framebuffer transformations, if has_nonstandard_transform is set */
    glm::mat4 transform = glm::mat4(1.0);

    /* The functions below to convert between coordinate systems don't need a
     * bound OpenGL context */
    /* Get the box after applying the framebuffer scale */
    wlr_box damage_box_from_geometry_box(wlr_box box) const;

    /* Get the projection of the given box onto the framebuffer.
     * The given box is in output-local coordinates, i.e the same coordinate
     * space as views */
    wlr_box framebuffer_box_from_geometry_box(wlr_box box) const;

    /* Get the projection of the given box onto the framebuffer.
     * The given box is in damage coordinates, e.g relative to the output's
     * framebuffer before rotation */
    wlr_box framebuffer_box_from_damage_box(wlr_box box) const;

    /* Returns a region in damage coordinate system which corresponds to the
     * whole area of the framebuffer */
    wf::region_t get_damage_region() const;

    /* Returns a matrix which contains an orthographic projection from "geometry"
     * coordinates to the framebuffer coordinates. */
    glm::mat4 get_orthographic_projection() const;
};
}

namespace OpenGL
{
    /* "Begin" rendering to the given framebuffer and the given viewport.
     * All rendering operations should happen between render_begin and render_end, because
     * that's the only time we're guaranteed we have a valid GLES context
     *
     * The other functions below assume they are called between render_begin()
     * and render_end() */
    void render_begin(); // use if you just want to bind GL context but won't draw
    void render_begin(const wf::framebuffer_base_t& fb);
    void render_begin(int32_t viewport_width, int32_t viewport_height, uint32_t fb = 0);

    /* Call this to indicate an end of the rendering.
     * Resets bound framebuffer and scissor box.
     * render_end() must be called for each render_begin() */
    void render_end();

    /* Clear the currently bound framebuffer with the given color */
    void clear(wf::color_t color, uint32_t mask = GL_COLOR_BUFFER_BIT);

    /* texg arguments are used only when bits has USE_TEX_GEOMETRY
     * if you don't wish to use them, simply pass {} as argument */
    void render_transformed_texture(GLuint text,
                                    const gl_geometry& g,
                                    const gl_geometry& texg,
                                    glm::mat4 transform = glm::mat4(1.0),
                                    glm::vec4 color = glm::vec4(1.f),
                                    uint32_t bits = 0);

    /* Compiles the given shader source */
    GLuint compile_shader(std::string source, GLuint type);

    /* Create a very simple gl program from the given shader sources */
    GLuint compile_program(std::string vertex_source, std::string frag_source);

    /**
     * Render a colored rectangle using OpenGL.
     *
     * @param box The rectangle geometry.
     * @param color The color of the rectangle.
     * @param matrix The matrix to transform the rectangle with.
     */
    void render_rectangle(wf::geometry_t box, wf::color_t color, glm::mat4 matrix);

}

/* utils */
glm::mat4 get_output_matrix_from_transform(wl_output_transform transform);

#endif // WF_OPENGL_HPP
