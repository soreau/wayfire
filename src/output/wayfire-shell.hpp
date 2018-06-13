#include <view.hpp>

struct wayfire_shell;
wayfire_shell *wayfire_shell_create(wl_display *display);

void wayfire_shell_handle_output_created(wayfire_output *output);
void wayfire_shell_handle_output_destroyed(wayfire_output *output);

void wayfire_shell_unmap_view(wayfire_view view);
