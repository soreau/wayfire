#include <opengl.hpp>
#include <output.hpp>
#include <core.hpp>
#include <render-manager.hpp>
#include <workspace-manager.hpp>
#include <animation.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <linux/input-event-codes.h>
#include <config.h>

#define COEFF_DELTA_NEAR 0.89567f
#define COEFF_DELTA_FAR  2.00000f

#if USE_GLES32
#include <GLES3/gl32.h>
#endif

class wayfire_cube : public wayfire_plugin_t {
    button_callback activate;
    wf_option act_button;

    std::vector<wf_workspace_stream*> streams;

    wf_option XVelocity, YVelocity, ZVelocity;

    float MaxFactor = 10;

    float angle;      // angle between sides
    float offset;     // horizontal rotation angle
    float offsetVert; // vertical rotation angle
    float zoomFactor = 1.0;

    int px, py;

    render_hook_t renderer;

    struct {
        GLuint id = -1;
        GLuint modelID, vpID;
        GLuint posID, uvID;
#if USE_GLES32
        GLuint easeID;
#endif
    } program;

    struct duple { float start, end; };

    struct
    {
        wf_transition offset_y, offset_z, rotation;
#if USE_GLES32
        wf_transition ease_deformation;
#endif

        bool in_exit, active = false;
    } animation;

    wf_duration duration;

    glm::mat4 vp, model, view, project;
    float coeff;

#if USE_GLES32
    wf_option use_light, use_deform;
    float current_ease;
#endif

    wf_option background_color;

    public:
    void init(wayfire_config *config)
    {
        grab_interface->name = "cube";
        grab_interface->abilities_mask = WF_ABILITY_CONTROL_WM;

        auto section = config->get_section("cube");

        XVelocity  = section->get_option("speed_spin_horiz", "0.01");
        YVelocity  = section->get_option("speed_spin_vert",  "0.01");
        ZVelocity  = section->get_option("speed_zoom",       "0.05");

        duration = wf_duration(section->get_option("initial_animation", "350"));
        background_color = section->get_option("background", "0 0 0 1");

        act_button = section->get_option("activate", "<alt> <ctrl> BTN_LEFT");
        if (!act_button->as_button().valid())
            return;

        activate = [=] (uint32_t, int32_t x, int32_t y) {
            initiate(x, y);
        };

        output->add_button(act_button, &activate);

        grab_interface->callbacks.pointer.button = [=] (uint32_t b, uint32_t s)
        {
            if (s == WL_POINTER_BUTTON_STATE_RELEASED)
                input_released();
        };

        grab_interface->callbacks.pointer.motion = [=] (int32_t x, int32_t y)
        {
            pointer_moved(x, y);
        };

        grab_interface->callbacks.pointer.axis = [=] (
                wlr_event_pointer_axis *ev) {
            if (ev->orientation == WLR_AXIS_ORIENTATION_VERTICAL)
                pointer_scrolled(ev->delta);
        };


#if USE_GLES32
        use_light  = section->get_option("light", "1");
        use_deform = section->get_option("deform", "1");
#endif

        auto vw = std::get<0>(output->workspace->get_workspace_grid_size());
        angle = 2 * M_PI / float(vw);
        coeff = 0.5 / std::tan(angle / 2);

        renderer = [=] (uint32_t fb) {render(fb);};
    }

    void schedule_next_frame()
    {
        output->render->schedule_redraw();
        output->render->damage(NULL);
    }

    void load_program()
    {
#if USE_GLES32
            std::string shaderSrcPath =
                INSTALL_PREFIX "/share/wayfire/cube/shaders_3.2";
#else
            std::string shaderSrcPath =
                INSTALL_PREFIX "/share/wayfire/cube/shaders_2.0";
#endif

            program.id = GL_CALL(glCreateProgram());
            GLuint vss, fss, tcs = -1, tes = -1, gss = -1;

            vss = OpenGL::load_shader(std::string(shaderSrcPath)
                        .append("/vertex.glsl").c_str(), GL_VERTEX_SHADER);

            fss = OpenGL::load_shader(std::string(shaderSrcPath)
                        .append("/frag.glsl").c_str(), GL_FRAGMENT_SHADER);

            GL_CALL(glAttachShader(program.id, vss));
            GL_CALL(glAttachShader(program.id, fss));

#if USE_GLES32
            tcs = OpenGL::load_shader(std::string(shaderSrcPath)
                    .append("/tcs.glsl").c_str(),
                    GL_TESS_CONTROL_SHADER);

            tes = OpenGL::load_shader(std::string(shaderSrcPath)
                    .append("/tes.glsl").c_str(),
                    GL_TESS_EVALUATION_SHADER);

            gss = OpenGL::load_shader(std::string(shaderSrcPath)
                    .append("/geom.glsl").c_str(),
                    GL_GEOMETRY_SHADER);
            GL_CALL(glAttachShader(program.id, tcs));
            GL_CALL(glAttachShader(program.id, tes));
            GL_CALL(glAttachShader(program.id, gss));
#endif

            GL_CALL(glLinkProgram(program.id));
            GL_CALL(glUseProgram(program.id));


            program.vpID = GL_CALL(glGetUniformLocation(program.id, "VP"));
            GL_CALL(glUniformMatrix4fv(program.vpID, 1, GL_FALSE, &vp[0][0]));

            program.uvID = GL_CALL(glGetAttribLocation(program.id, "uvPosition"));
            program.posID = GL_CALL(glGetAttribLocation(program.id, "position"));
            program.modelID = GL_CALL(glGetUniformLocation(program.id, "model"));

#if USE_GLES32
            GLuint defID = GL_CALL(glGetUniformLocation(program.id, "deform"));
            glUniform1i(defID, *use_deform);

            GLuint lightID = GL_CALL(glGetUniformLocation(program.id, "light"));
            glUniform1i(lightID, *use_light);

            program.easeID = GL_CALL(glGetUniformLocation(program.id, "ease"));
#endif

            GetTuple(vw, vh, output->workspace->get_workspace_grid_size());
            (void) vh;

            streams.resize(vw);


            for(int i = 0; i < vw; i++) {
                streams[i] = new wf_workspace_stream;
                streams[i]->fbuff = streams[i]->tex = -1;
            }

            project = glm::perspective(45.0f, 1.f, 0.1f, 100.f);
    }

    void initiate(int x, int y)
    {
        if (!animation.active)
        {
            if (!output->activate_plugin(grab_interface))
                return;

            grab_interface->grab();
            output->render->set_renderer(renderer);
        }

        animation.in_exit = false;
        animation.active = true;

        offset = 0;
        offsetVert = 0;
        zoomFactor = 1;
        duration.start();
        animation.in_exit = false;
        animation.offset_z = {coeff + COEFF_DELTA_NEAR, coeff + COEFF_DELTA_FAR};

#if USE_GLES32
        animation.ease_deformation = {0, 1};
#endif

        if (update_animation())
            schedule_next_frame();

        px = x;
        py = y;
    }

    bool update_animation()
    {
        float z_offset = duration.progress(animation.offset_z);

#if USE_GLES32
        current_ease = duration.progress(animation.ease_deformation);
#endif

        /* also update rotation and Y offset */
        if (animation.in_exit)
        {
            offsetVert = duration.progress(animation.offset_y);
            offset = duration.progress(animation.rotation);
        }

        view = glm::lookAt(glm::vec3(0., offsetVert, z_offset),
                glm::vec3(0., 0., 0.),
                glm::vec3(0., 1., 0.));

        return duration.running();
    }

    void render(uint32_t target_fb)
    {
        if (program.id == (uint)-1)
            load_program();

        OpenGL::use_device_viewport();

        auto clear_color = background_color->as_cached_color();
        GL_CALL(glClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a));

        GL_CALL(glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT));

        GetTuple(vx, vy, output->workspace->get_current_workspace());
        for(size_t i = 0; i < streams.size(); i++) {
            if (!streams[i]->running) {
                streams[i]->ws = std::make_tuple(i, vy);
                output->render->workspace_stream_start(streams[i]);
            } else {
                output->render->workspace_stream_update(streams[i]);
            }
        }

        GL_CALL(glUseProgram(program.id));
        GL_CALL(glEnable(GL_DEPTH_TEST));
        GL_CALL(glDepthFunc(GL_LESS));

        OpenGL::use_device_viewport();
        GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fb));
        vp = project * view;
        GL_CALL(glUniformMatrix4fv(program.vpID, 1, GL_FALSE, &vp[0][0]));

#if USE_GLES32
        GL_CALL(glUniform1f(program.easeID, current_ease));
#endif

        glm::mat4 base_model = glm::scale(glm::mat4(1.0),
                glm::vec3(1. / zoomFactor, 1. / zoomFactor,
                    1. / zoomFactor));


        GLfloat vertexData[] = {
            -0.5,  0.5,
             0.5,  0.5,
             0.5, -0.5,
            -0.5,  0.5,
             0.5, -0.5,
            -0.5, -0.5
        };

        GLfloat coordData[] = {
            0.0f, 1.0f,
            1.0f, 1.0f,
            1.0f, 0.0f,
            0.0f, 1.0f,
            1.0f, 0.0f,
            0.0f, 0.0f,
        };

        GL_CALL(glVertexAttribPointer(program.posID, 2, GL_FLOAT, GL_FALSE, 0, vertexData));
        GL_CALL(glEnableVertexAttribArray(program.posID));

        GL_CALL(glVertexAttribPointer(program.uvID, 2, GL_FLOAT, GL_FALSE, 0, coordData));
        GL_CALL(glEnableVertexAttribArray(program.uvID));

        wlr_renderer_scissor(core->renderer, NULL);
        for(size_t i = 0; i < streams.size(); i++) {
            int index = (vx + i) % streams.size();

            GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
            GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

            GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
            GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));

            GL_CALL(glBindTexture(GL_TEXTURE_2D, streams[index]->tex));
            GL_CALL(glActiveTexture(GL_TEXTURE0));

            model = glm::rotate(base_model,
                    float(i) * angle + offset, glm::vec3(0, 1, 0));
            model = glm::translate(model, glm::vec3(0, 0, coeff));
           GL_CALL(glUniformMatrix4fv(program.modelID, 1, GL_FALSE, &model[0][0]));

#if USE_GLES32
           GL_CALL(glDrawArrays(GL_PATCHES, 0, 6));
#else
           GL_CALL(glDrawArrays(GL_TRIANGLES, 0, 6));
#endif
        }
        glDisable(GL_DEPTH_TEST);

        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        GL_CALL(glDisableVertexAttribArray(program.posID));
        GL_CALL(glDisableVertexAttribArray(program.uvID));

        bool result = update_animation();
        if (result)
            schedule_next_frame();

        if (animation.in_exit && !result)
            terminate();
    }

    void input_released()
    {
        int size = streams.size();

        float dx = -(offset) / angle;
        int dvx = std::floor(dx + 0.5);

        GetTuple(vx, vy, output->workspace->get_current_workspace());
        int nvx = (vx + (dvx % size) + size) % size;
        output->workspace->set_workspace(std::make_tuple(nvx, vy));

        duration.start();
        animation.in_exit = true;
        animation.offset_z = {coeff + COEFF_DELTA_FAR, coeff + COEFF_DELTA_NEAR};
        animation.offset_y = {offsetVert, 0};
        animation.rotation = {offset + 1.0f * dvx * angle, 0};

#if USE_GLES32
        animation.ease_deformation = {1, 0};
#endif

        update_animation();
        schedule_next_frame();
    }

    void terminate()
    {
        animation.active = false;
        animation.in_exit = false;

        output->render->reset_renderer();
        output->deactivate_plugin(grab_interface);

        auto size = streams.size();
        for (uint i = 0; i < size; i++)
            output->render->workspace_stream_stop(streams[i]);
    }

    void pointer_moved(int x, int y)
    {
        if (animation.in_exit)
            return;

        int xdiff = x - px;
        int ydiff = y - py;
        offset += xdiff * XVelocity->as_cached_double();
        offsetVert += ydiff * YVelocity->as_cached_double();
        px = x, py = y;

        schedule_next_frame();
    }

    void pointer_scrolled(double amount)
    {
        zoomFactor += amount * ZVelocity->as_cached_double();

        if (zoomFactor > MaxFactor)
            zoomFactor = MaxFactor;

        if (zoomFactor <= 0.1)
            zoomFactor = 0.1;

        schedule_next_frame();
    }
};

extern "C" {
    wayfire_plugin_t *newInstance() {
        return new wayfire_cube();
    }

}
