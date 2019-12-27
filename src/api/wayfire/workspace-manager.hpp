#ifndef WORKSPACE_MANAGER_HPP
#define WORKSPACE_MANAGER_HPP

#include <functional>
#include <vector>
#include <wayfire/view.hpp>

namespace wf
{
/**
 * The workspace implementation is a way for plugins to request more detailed
 * control over what happens on the given workspace. For example a tiling
 * plugin would disable move and/or resize operations for some views.
 */
struct workspace_implementation_t
{
    virtual bool view_movable(wayfire_view view) = 0;
    virtual bool view_resizable(wayfire_view view) = 0;
    virtual ~workspace_implementation_t() {}
};

/**
 * Wayfire organizes views into several layers, in order to simplify z ordering.
 */
enum layer_t
{
    /* The lowest layer, typical clients here are backgrounds */
    LAYER_BACKGROUND = (1 << 0),
    /* The bottom layer */
    LAYER_BOTTOM     = (1 << 1),
    /* The workspace layer is where regular views are placed */
    LAYER_WORKSPACE  = (1 << 2),
    /* The xwayland layer is used for Xwayland O-R windows */
    LAYER_XWAYLAND   = (1 << 3),
    /* The top layer. Typical clients here are non-autohiding panels */
    LAYER_TOP        = (1 << 4),
    /* The fullscreen layer, used only for fullscreen views */
    LAYER_FULLSCREEN = (1 << 5),
    /* The lockscreen layer, typically lockscreens or autohiding panels */
    LAYER_LOCK       = (1 << 6),
    /* The layer where "desktop widgets" are positioned, for example an OSK
     * or a sound control popup */
    LAYER_DESKTOP_WIDGET = (1 << 7),

    /* The minimized layer. It has no z order since it is not visible at all */
    LAYER_MINIMIZED  = (1 << 8)
};

constexpr int TOTAL_LAYERS = 9;

/* The layers where regular views are placed */
constexpr int WM_LAYERS     = (wf::LAYER_WORKSPACE  | wf::LAYER_FULLSCREEN);
/* All layers which are used for regular clients */
constexpr int MIDDLE_LAYERS = (wf::WM_LAYERS        | wf::LAYER_XWAYLAND);
/* All layers which typically sit on top of other layers */
constexpr int ABOVE_LAYERS  = (wf::LAYER_TOP        | wf::LAYER_LOCK |
                               wf::LAYER_DESKTOP_WIDGET);
/* All layers which typically sit below other layers */
constexpr int BELOW_LAYERS  = (wf::LAYER_BACKGROUND | wf::LAYER_BOTTOM);

/* All visible layers */
constexpr int VISIBLE_LAYERS = (wf::MIDDLE_LAYERS | wf::ABOVE_LAYERS |
                                wf::BELOW_LAYERS);
/* All layers */
constexpr int ALL_LAYERS     = (wf::VISIBLE_LAYERS | wf::LAYER_MINIMIZED);

/**
 * @return A bitmask consisting of all layers which are not below the given layer
 */
uint32_t all_layers_not_below(uint32_t layer);

/**
 * Workspace manager is responsible for managing the layers, the workspaces and
 * the views in them. There is one workspace manager per output.
 *
 * In the default workspace_manager implementation, there is one set of layers
 * per output. Each layer is infinite and covers all workspaces.
 *
 * Each output also has a set of workspaces, arranged in a 2D grid. A view may
 * overlap multiple workspaces.
 */
class workspace_manager
{
  public:
    /**
     * Check if the given view is visible on the given workspace
     */
    bool view_visible_on(wayfire_view view, wf_point ws);

    /**
     * Get a list of all views visible on the given workspace
     *
     * @param layer_mask - The layers whose views should be included
     * @param wm_only - If set to true, then only the view's wm geometry
     *        will be taken into account when computing visibility.
     */
    std::vector<wayfire_view> get_views_on_workspace(wf_point ws,
        uint32_t layer_mask, bool wm_only);

    /**
     * Ensure that the view's wm_geometry is visible on the workspace ws. This
     * involves moving the view as appropriate.
     */
    void move_to_workspace(wayfire_view view, wf_point ws);

    /**
     * Add the given view to the given layer. If the view was already added to
     * a layer, it will be first removed from the old one.
     *
     * Preconditions: the view must have the same output as the current one
     */
    void add_view(wayfire_view view, layer_t layer);

    /**
     * Bring the view to the top of its layer. No-op if the view isn't in any
     * layer.
     */
    void bring_to_front(wayfire_view view);

    /**
     * Restack the view on top of the given view. The stacking order of other
     * views is left unchanged
     */
    void restack_above(wayfire_view view, wayfire_view below);

    /**
     * Restack the view below the given view. The stacking order of other views
     * is left unchanged
     */
    void restack_below(wayfire_view view, wayfire_view above);

    /**
     * Remove the view from its layer. This effectively means that the view is
     * now invisible on the output.
     */
    void remove_view(wayfire_view view);

    /**
     * @return The layer in which the view is, or 0 if it can't be found
     */
    uint32_t get_view_layer(wayfire_view view);

    /**
     * @return A list of all views in the given layers.
     */
    std::vector<wayfire_view> get_views_in_layer(uint32_t layers_mask);

    /**
     * @return The current workspace implementation
     */
    workspace_implementation_t* get_workspace_implementation();

    /**
     * Set the active workspace implementation
     * @param impl - The workspace implementation, or null if default
     * @param overwrite - Whether to set the implementation even if another
     *        non-default implementation has already been set.
     *
     * @return true iff the implementation has been set
     */
    bool set_workspace_implementation(
        std::unique_ptr<workspace_implementation_t> impl,
        bool overwrite = false);

    /**
     * Change the active workspace.
     *
     * @param The new active workspace.
     */
    void set_workspace(wf_point ws);

    /**
     * @return The given workspace
     */
    wf_point get_current_workspace();

    /**
     * @return The number of workspace columns and rows
     */
    wf_size_t get_workspace_grid_size();

    /**
     * Special clients like panels can reserve place from an edge of the output.
     * It is used when calculating the dimensions of maximized/tiled windows and
     * others. The remaining space (which isn't reserved for panels) is called
     * the workarea.
     */
    enum anchored_edge
    {
        ANCHORED_EDGE_TOP = 0,
        ANCHORED_EDGE_BOTTOM = 1,
        ANCHORED_EDGE_LEFT = 2,
        ANCHORED_EDGE_RIGHT = 3
    };

    struct anchored_area
    {
        /* The edge from which to reserver area */
        anchored_edge edge;
        /* Amount of space to reserve */
        int reserved_size;

        /* Desired size, to be given later in the reflowed callback */
        int real_size;

        /* The reflowed callbacks allows the component registering the
         * anchored area to be notified whenever the dimensions or the position
         * of the anchored area changes.
         *
         * The first passed geometry is the geometry of the anchored area. The
         * second one is the available workarea at the moment that the current
         * workarea was considered. */
        std::function<void(wf_geometry, wf_geometry)> reflowed;
    };

    /**
     * Add a reserved area. The actual recalculation must be manually
     * triggered by calling reflow_reserved_areas()
     */
    void add_reserved_area(anchored_area *area);

    /**
     * Remove a reserved area. The actual recalculation must be manually
     * triggered by calling reflow_reserved_areas()
     */
    void remove_reserved_area(anchored_area *area);

    /**
     * Recalculate reserved area for each anchored area
     */
    void reflow_reserved_areas();

    /**
     * @return The free space of the output after reserving the space for panels
     */
    wf_geometry get_workarea();

    workspace_manager(output_t *output);
    ~workspace_manager();
  protected:
    class impl;
    std::unique_ptr<impl> pimpl;
};
}

#endif /* end of include guard: WORKSPACE_MANAGER_HPP */
