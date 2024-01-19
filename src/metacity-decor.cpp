#include <linux/input-event-codes.h>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/plugins/common/simple-texture.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/view.hpp>
#include <wayfire/workarea.hpp>
#include <wayfire/matcher.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/output.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/txn/transaction-manager.hpp>
#include <wayfire/txn/transaction.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/window-manager.hpp>

#include "wayfire/core.hpp"
#include "wayfire/signal-provider.hpp"
#include "wayfire/toplevel-view.hpp"
#include "wayfire/toplevel.hpp"
#include <GLES2/gl2ext.h>
#include <wayfire/plugins/common/cairo-util.hpp>

#include <stdio.h>
#include "nonstd.hpp"

#define MODE_HOVER   0
#define MODE_CLICK   1
#define MODE_RELEASE 2

static wf::option_wrapper_t<std::string> theme{"metacity-decor/metatheme"};
static wf::option_wrapper_t<std::string> font{"metacity-decor/font"};
static wf::option_wrapper_t<std::string> buttons{"metacity-decor/button_layout"};
static wf::option_wrapper_t<std::string> dialog_buttons{"metacity-decor/dialog_button_layout"};
static wf::option_wrapper_t<wf::color_t> background_color{"metacity-decor/background_color"};
static wf::option_wrapper_t<wf::color_t> text_color{"metacity-decor/text_color"};

extern GdkRGBA title_color, bg_color;

MetaTheme               *metatheme = NULL;
MetaButtonLayout        button_layout, dialog_button_layout;
PangoFontDescription    *font_desc;
int                     text_height;

static wf::decoration_margins_t deco_margins =
    {
        .left = 4,
        .right = 4,
        .bottom = 4,
        .top = 24,
};
static int borders_delta = 5;

class simple_decoration_node_t : public wf::scene::node_t, public wf::pointer_interaction_t
{
public:
    uint32_t state = 0;

private:    
    std::weak_ptr<wf::toplevel_view_interface_t> _view;
    
    wf::region_t cached_region;
    wf::dimensions_t size = {1, 1};
    wf::dimensions_t orig_size = {1, 1};
    cairo_surface_t *surface[2] = {NULL, NULL};
    wlr_box area;
    wf::wl_timer<false> refresh_timer;
    MetaFrameGeometry frame_geometry;
    MetaButtonState button_states[META_BUTTON_TYPE_LAST];
    MetaButtonFunction last_active_button = META_BUTTON_FUNCTION_LAST; 
    MetaButtonFunction last_pressed_button = META_BUTTON_FUNCTION_LAST;
    int is_grabbed = 0;
    int last_hover_state = 0;
    int current_x, current_y, current_ms, may_be_hover = 0;

    wf::signal::connection_t<wf::view_minimized_signal> on_minimized = [=, this] (wf::view_minimized_signal *ev) {
        reset_button_states ();
    };

    wf::signal::connection_t<wf::view_title_changed_signal> title_set = [=, this] (wf::view_title_changed_signal *ev) {
        if (auto view = _view.lock()) {
            resize (size);
        }
    };
    
    void reset_button_states ()
    {
        for (int i = 0; i < META_BUTTON_TYPE_LAST; i++)
            button_states[i] = META_BUTTON_STATE_NORMAL;
        last_active_button = last_pressed_button = META_BUTTON_FUNCTION_LAST;
        last_hover_state = 0;
    }

    wf::point_t get_offset() 
    {
        if(state & STATE_MAXIMIZED)
            return { -(deco_margins.left - borders_delta), -(deco_margins.top - borders_delta)};
        else
            return { -deco_margins.left, -deco_margins.top };
    }

    void render_scissor_box(const wf::render_target_t& fb, wf::point_t origin,
                            const wlr_box& scissor) 
    {   
        auto view = _view.lock();
        // Draw the background
        wlr_box geometry{origin.x, origin.y, size.width, size.height};
        wlr_box clip = scissor;
        if (!wlr_box_intersection(&clip, &scissor, &geometry)) {
            return;
        }
        wf::point_t o = { origin.x, origin.y };
        wf::color_t bg{0,0,0,0};
        uint32_t bits = OpenGL::TEXTURE_TRANSFORM_INVERT_Y;
        OpenGL::render_begin(fb);
        fb.logic_scissor(scissor);
        // draw a transparent background
        OpenGL::render_rectangle(clip, bg, fb.get_orthographic_projection());
        wf::simple_texture_t texture;
        int what = view->activated ? 1 : 0;
        cairo_surface_upload_to_texture(surface[what], texture);
        OpenGL::render_texture(texture.tex, fb, area + o, glm::vec4(1.0f), bits);
        OpenGL::render_end();
    }

    wf::region_t calculate_region(wf::dimensions_t dims) const 
    {
        wf::region_t r{};
        r |= area;
        return r;
    }
    
    class decoration_render_instance_t : public wf::scene::render_instance_t
    {
        simple_decoration_node_t *self;
        wf::scene::damage_callback push_damage;
        wf::signal::connection_t<wf::scene::node_damage_signal> on_surface_damage =
            [=, this] (wf::scene::node_damage_signal *data) {
                push_damage(data->region);
            };

        public:
 
      
        decoration_render_instance_t(simple_decoration_node_t *self, wf::scene::damage_callback push_damage)
        {
            this->self = self;
            this->push_damage = push_damage;
            self->connect(&on_surface_damage);
        }

        void schedule_instructions(std::vector<wf::scene::render_instruction_t>& instructions,
                                   const wf::render_target_t& target, wf::region_t& damage) override
        {
            auto our_region = self->cached_region + self->get_offset();
            wf::region_t our_damage = damage & our_region;
            if (!our_damage.empty()) {
                instructions.push_back(wf::scene::render_instruction_t{
                        .instance = this,
                        .target   = target,
                        .damage   = std::move(our_damage),
                    });
            }
        }

        void render(const wf::render_target_t& target,
                    const wf::region_t& region) override
        {
            for (const auto& box : region) {
                self->render_scissor_box(target, self->get_offset(), wlr_box_from_pixman_box(box));
            }
        }
    };

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
                              wf::scene::damage_callback push_damage, wf::output_t *output = nullptr) override
    {
        instances.push_back(std::make_unique<decoration_render_instance_t>(this, push_damage));
    }

    std::optional<wf::scene::input_node_t> find_node_at(const wf::pointf_t& at) override 
    {
        wf::pointf_t local = at - wf::pointf_t{get_offset()};
        if (cached_region.contains_pointf(local)) 
        {
            return wf::scene::input_node_t{
                .node = this,
                .local_coords = local,
            };
        }

        return {};
    }

    pointer_interaction_t& pointer_interaction() override {
        return *this;
    }

    /* wf::compositor_surface_t implementation */
    void handle_pointer_enter(wf::pointf_t point) override {
        point -= wf::pointf_t{get_offset()};
        current_x = point.x;
        current_y = point.y;
        update_cursor ();
    }

    void handle_pointer_leave() override {
        if (may_be_hover)
        {
            resize( size);
        }
        may_be_hover = 0;
        update_cursor ();
    }

    bool check_button (int mode, int x, int y, MetaButtonState state, int pressed, MetaButtonFunction *what)
    {
        *what = META_BUTTON_FUNCTION_LAST;
        MetaButtonFunction *which_buttons;
        if (mode == MODE_RELEASE)
        {
            if (last_hover_state && last_active_button != META_BUTTON_FUNCTION_LAST)
            {
                button_states[meta_function_to_type(last_active_button)] = META_BUTTON_STATE_PRELIGHT;
                *what = last_active_button;
                last_active_button = META_BUTTON_FUNCTION_LAST;
                last_pressed_button = META_BUTTON_FUNCTION_LAST;
                last_hover_state = 0;
                return true;
            }
        }
        which_buttons = x < frame_geometry.title_rect.x ? &button_layout.left_buttons[0] : &button_layout.right_buttons[0];
        for (int i = 0; which_buttons[i] != META_BUTTON_FUNCTION_LAST; i++)
        {
            int rx,ry,rw,rh;
            if (!meta_get_button_position (which_buttons[i], &frame_geometry, &rx,&ry,&rw,&rh))
            {
                continue;
            }
            if (x >= rx && x <= rx + rw)
            {
                if (mode == MODE_HOVER && which_buttons[i] == last_active_button)
                {   
                    return true;
                }
                last_hover_state = 1;
                button_states[meta_function_to_type(last_active_button)] = META_BUTTON_STATE_NORMAL;
                button_states[meta_function_to_type(which_buttons[i])] = state;
                last_active_button = which_buttons[i];
                if (pressed)
                {
                    LOGI("button ", meta_button_function_to_string(which_buttons[i]), " click");
                    last_pressed_button = which_buttons[i];
                }
                else
                {
                    LOGI("button ", meta_button_function_to_string(which_buttons[i]), " hover");
                }
                return true;
            }
        }
        return false;
    }

    void update_buttons(int pressed)
    {
        MetaButtonFunction what;
        int x = current_x;
        int y = current_y;
        if (!pressed) 
        {
            if (last_pressed_button != META_BUTTON_FUNCTION_LAST)
            {
                if (check_button (MODE_RELEASE, x, y, META_BUTTON_STATE_PRESSED, pressed, &what))
                {
                    const char *action = meta_button_function_to_string (what);
                    LOGI("action ", x, " ", y, " ", action);
                    reset_button_states();
                    handle_action (action, 0);
                }
                resize (size);
            }
            else
            {
                if (check_button (MODE_HOVER, x, y, META_BUTTON_STATE_PRELIGHT, pressed, &what))
                {
                    resize (size);
                }                    
            }
        }
        else
        {
            if (check_button (MODE_CLICK, x, y, META_BUTTON_STATE_PRESSED, pressed, &what))
            {
                resize (size);
            }
        }                
    }
        
    void handle_pointer_motion(wf::pointf_t to, uint32_t time_ms) override 
    {
        to -= wf::pointf_t{get_offset()};
        current_x = to.x;
        current_y = to.y;
        current_ms = time_ms;    
        uint32_t edges = update_cursor ();
        if (!edges)
        {
            if (current_x > frame_geometry.title_rect.x && 
                current_x < frame_geometry.title_rect.x + frame_geometry.title_rect.width)
            {
                // within title rect
                reset_button_states ();
                resize(size);
                may_be_hover = 0;
            }
            else
            {
                update_buttons (is_grabbed);
            }
        }
    }

    void handle_pointer_button(const wlr_pointer_button_event& ev) override 
    {
        if (ev.button != BTN_LEFT)
        {
            return;
        }
        LOGI("button_press ",ev.button," ", ev.state);
        is_grabbed = ev.state;
        uint32_t edge = calculate_resize_edges ();
        if (is_grabbed)
        {
            if (edge)
                handle_action ("resize", edge);
            else
            {
                if (current_x > frame_geometry.title_rect.x && 
                    current_x < frame_geometry.title_rect.x + frame_geometry.title_rect.width)
                {
                    handle_action ("move", 0);
                }
                else
                {
                    update_buttons (is_grabbed);
                }
            }
        }        
        else
        {
            update_buttons (is_grabbed);
        }
    }
    std::shared_ptr<wf::scene::node_t> main_node;

/*    

Disclaimer: to achieve shade/unshade I resorted to a dirty trick with a caveat...
To shade, I remove from the toplevel the decorated view's node and re add it to unshade.
The caveat is: if a view is shaded, to maximize/unmaximize it I unshade before proceeding
because if not UGLY things will happen...
Also, a shaded view is not resizable.
Maybe there is a more elegant and robust way to achieve this with transactions and/or 
additional nodes, even with animation.
But I'm pretty new to wayfire, up to now my knowledge reaches so far...

*/    
    void handle_action(const char* action, uint32_t edges) 
    {
        reset_button_states ();
        if (auto view = _view.lock()) 
        {
            LOGI("action " , action);
            if (strcmp (action, "move") == 0)
            {
                wf::get_core().default_wm->move_request(view);
            }
            else if (strcmp (action, "resize") == 0)
            {
                wf::get_core().default_wm->resize_request(view, edges);
            }
            else if (strcmp (action, "minimize") == 0)
            {
                wf::get_core().default_wm->minimize_request(view, true);
            }
            else if (strcmp (action, "close") == 0)
            {
                view->close();
            }
            else if (strcmp (action, "stick") == 0)
            {
                view->set_sticky(1);
            }
            else if (strcmp (action, "unstick") == 0)
            {
                view->set_sticky(0);
            }
            else if (strcmp (action, "maximize") == 0)
            {
                if(shaded)
                {
                    handle_action("unshade",0);
                }
                if (view->pending_tiled_edges()) 
                {
                    wf::get_core().default_wm->tile_request(view, 0);
                } 
                else 
                {
                    wf::get_core().default_wm->tile_request(view, wf::TILED_EDGES_ALL);
                }
            }
            else if (strcmp (action, "shade") == 0)
            {
                main_node = view->get_surface_root_node()->get_children()[0];
                wf::scene::remove_child(main_node, 0);
                orig_size = size;
                shaded = true;
                resize({size.width, deco_margins.top + deco_margins.bottom});
            }
            else if (strcmp (action, "unshade") == 0)
            {
                wf::scene::add_front(view->get_surface_root_node(), main_node);
                shaded = false;
                size = orig_size;
                resize(size);
            }
        }
    }

    uint32_t calculate_resize_edges()
    {
        uint32_t edge = 0; 
        if (state & STATE_MAXIMIZED || shaded)
            return edge;
        auto g = get_bounding_box();
        if ( current_x < deco_margins.left )
            edge |= WLR_EDGE_LEFT;
        if ( current_x > g.width - deco_margins.right)
            edge |= WLR_EDGE_RIGHT;
        if ( current_y > g.height - deco_margins.bottom)
            edge |= WLR_EDGE_BOTTOM;
        if ( current_y < frame_geometry.title_rect.y)
            edge |= WLR_EDGE_TOP;
        return edge;
    }
    
    // Update the cursor based on @current_input
    uint32_t update_cursor() 
    {
        uint32_t edges = calculate_resize_edges();
        auto cursor_name = edges > 0 ?
            wlr_xcursor_get_resize_name((wlr_edges)edges) : "default";
        wf::get_core().set_cursor(cursor_name);
        return edges;
    }

public:
    bool shaded = false;

    simple_decoration_node_t(wayfire_toplevel_view view)
        : node_t(false)
    {
        this->_view = view->weak_from_this();
        if(view->toplevel()->pending().tiled_edges)
            state = STATE_MAXIMIZED;
        view->connect(&title_set);
        reset_button_states ();
    }

    ~simple_decoration_node_t ()
    {
        for (int i = 0; i < 2; i++)
        {
            if (surface[i])
                cairo_surface_destroy (surface[i]);
        }
    }

    void resize(wf::dimensions_t dims) 
    {
        if(dims.width == 0 || dims.height == 0)
            return;
        auto view = _view.lock();
        if (!view)
            return;
        view->damage();

        size = dims;
        bool maximized = view->toplevel()->pending().tiled_edges; 

        if(shaded)
        {
            // may be was resized by the client and the view was readded
            auto cl = view->get_surface_root_node()->get_children();
            if(cl.size() > 1) 
            {
                // view was readded, unshade
                shaded = false;
                size = orig_size;
                refresh_timer.set_timeout(50, [=] ()
                {
                    resize(size);
                });
            }
        }            
            
        area = wlr_box{0, 0, size.width, size.height};

        int width,height;
        width = size.width - deco_margins.left - deco_margins.right;
        height = size.height - deco_margins.top - deco_margins.bottom;
        if (maximized)
        {
            state |= STATE_MAXIMIZED;
            width += (borders_delta * 2);
            height += (borders_delta * 2);
        }
        else
            state &= ~STATE_MAXIMIZED;

        //LOGI("resize ", view->get_title(), " ", dims, " ", width, " ", height);
        std::string title = view->get_title();
        for (int i = 0; i < 2; i++)
        {
            if (surface[i])
                cairo_surface_destroy (surface[i]);
            surface[i] = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size.width, size.height);
            cairo_t *cr = cairo_create (surface[i]);
            PangoLayout* layout = pango_cairo_create_layout(cr);
            pango_layout_set_font_description(layout, font_desc);  
            pango_layout_set_wrap (layout, PANGO_WRAP_CHAR);
            pango_layout_set_auto_dir (layout, FALSE);
            pango_layout_set_width (layout, -1);
            if(metatheme == NULL)
                pango_layout_set_text (layout, "Theme not found !", -1);
            else                
                pango_layout_set_text (layout, title.c_str(), -1);
            cairo_set_source_rgba (cr, 0, 0, 0, 0);
            cairo_paint(cr);
            if(metatheme == NULL)
            {
                wf::color_t fg = (wf::color_t)text_color;
                cairo_set_source_rgba (cr, fg.r, fg.g, fg.b, fg.a);
                cairo_move_to(cr, 5, 3);
                pango_cairo_show_layout(cr, layout);
            }
            else
            {
                uint32_t frame_state = i ? STATE_FOCUSED : 0;
                if (shaded )
                    frame_state |= STATE_SHADED;
                if (maximized)
                {
                    frame_state |= STATE_MAXIMIZED;
                }
                if (view->sticky)
                {
                    frame_state |= STATE_STICKY;
                }
                meta_theme_draw_frame (metatheme, frame_state, NULL, cr, width, height, layout, 
                                       text_height, &frame_geometry, 
                                       view->parent == nullptr ? &button_layout : &dialog_button_layout,
                                       button_states);
            }                                   
            cairo_destroy (cr);
        }                        

        if (!view->toplevel()->current().fullscreen) {
            this->cached_region = calculate_region (size);
        }
        view->damage();
    }

    wf::geometry_t get_bounding_box() override
    {
        return wf::construct_box(get_offset(), size);
    }
   
};
    
class simple_decorator_t : public wf::custom_data_t 
{
public:
    wayfire_toplevel_view view;
    std::shared_ptr<simple_decoration_node_t> deco;

    wf::signal::connection_t<wf::view_activated_state_signal> on_view_activated;
    wf::signal::connection_t<wf::view_geometry_changed_signal> on_view_geometry_changed;
    wf::signal::connection_t<wf::view_fullscreen_signal> on_view_fullscreen;
    wlr_surface *decorated_surface;
    
    simple_decorator_t(wayfire_toplevel_view view) 
    {
        this->view = view;
        deco       = std::make_shared<simple_decoration_node_t>(view);
        deco->resize(wf::dimensions(view->get_pending_geometry()));
        wf::scene::add_back(view->get_surface_root_node(), deco);

        view->connect(&on_view_activated);
        view->connect(&on_view_geometry_changed);
        view->connect(&on_view_fullscreen);

        on_view_activated = [this] (auto) {
            wf::scene::damage_node(deco, deco->get_bounding_box());
        };

        on_view_geometry_changed = [this] (auto) 
        {
            if (deco->shaded)
                return;
            auto geo = this->view->get_geometry();
            deco->resize(wf::dimensions(geo));
        };

        on_view_fullscreen = [this] (auto) 
        {
            if (!this->view->toplevel()->current().fullscreen) 
            {
                deco->resize(wf::dimensions(this->view->get_geometry()));
            }
        };
    }

    ~simple_decorator_t() 
    {
        wf::scene::remove_child( deco );
        LOGI("simple_decorator_t deleted");
    }

    wf::decoration_margins_t get_margins(const wf::toplevel_state_t& pending)
    {
        if (pending.fullscreen) {
            return {0, 0, 0, 0};
        }
        uint edges = pending.tiled_edges;
        wf::decoration_margins_t margins;
        margins.top = deco_margins.top - (edges & WLR_EDGE_TOP ? borders_delta : 0);
        margins.left = deco_margins.left - (edges & WLR_EDGE_LEFT ? borders_delta : 0);
        margins.right = deco_margins.right - (edges & WLR_EDGE_RIGHT ? borders_delta : 0);
        margins.bottom = deco_margins.bottom - (edges & WLR_EDGE_BOTTOM ? borders_delta : 0);
        return margins;
    }
};
    
class wayfire_metacity_decor_t : public wf::plugin_interface_t 
{
    wf::view_matcher_t ignore_views{"metacity-decor/ignore_views"};

    wf::signal::connection_t<wf::txn::new_transaction_signal> on_new_tx = [this] (wf::txn::new_transaction_signal *ev) {
        // For each transaction, we need to consider what happens with participating views
        for (const auto& obj : ev->tx->get_objects()) {
            if (auto toplevel = std::dynamic_pointer_cast<wf::toplevel_t>(obj)) {
                // First check whether the toplevel already has decoration
                // In that case, we should just set the correct margins
                
                auto view = wf::find_view_for_toplevel(toplevel);
                if (auto deco = toplevel->get_data<simple_decorator_t>()) {
                    toplevel->pending().margins = deco->get_margins(toplevel->pending());
                    continue;
                }

                // Second case: the view is already mapped, or the transaction does not map it.
                // The view is not being decorated, so nothing to do here.
                if (toplevel->current().mapped || !toplevel->pending().mapped)
                {
                    continue;
                }

                // Third case: the transaction will map the toplevel.
                if(view->get_title() == "nil")
                    continue;

                wf::dassert(view != nullptr, "Mapping a toplevel means there must be a corresponding view!");
                if (should_decorate_view(view))
                {
                    adjust_new_decorations(view);
                }
            }
        }
    };

    wf::signal::connection_t<wf::view_decoration_state_updated_signal> on_decoration_state_updated =
        [this] (wf::view_decoration_state_updated_signal *ev) {
            update_view_decoration(ev->view);
        };
    bool config_changed = false;
    
    void setup ()
    {
        std::string stdstr = (std::string)theme;
        meta_theme_set_current(stdstr.c_str(), TRUE);
        metatheme = meta_theme_get_current();
        if(metatheme == NULL)
        {
            LOGI("************ Theme ",stdstr," not found ! ************");
            return;
        }            
        stdstr = (std::string)buttons;
        meta_update_button_layout (stdstr.c_str(), &button_layout);
        stdstr = (std::string)dialog_buttons;
        meta_update_button_layout (stdstr.c_str(), &dialog_button_layout);
        stdstr = (std::string)font;
        font_desc = pango_font_description_from_string(stdstr.c_str());
        wf::color_t color = (wf::color_t)text_color;
        title_color.red = color.r;
        title_color.green = color.g;
        title_color.blue = color.b;
        title_color.alpha = color.a;
        color = (wf::color_t)background_color;
        bg_color.red = color.r;
        bg_color.green = color.g;
        bg_color.blue = color.b;
        bg_color.alpha = color.a;        
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 300, 300);
        cairo_t *cr = cairo_create(surface);
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, font_desc);  
	    pango_layout_set_wrap (layout, PANGO_WRAP_CHAR);
        pango_layout_set_auto_dir (layout, FALSE);
        pango_layout_set_width (layout, -1);
        pango_layout_set_text (layout, "Prova", -1);
        pango_layout_get_pixel_size (layout, NULL, &text_height);
        MetaFrameGeometry fgeom;

        meta_theme_draw_frame_test (metatheme, NULL, 300, 300, text_height, 
                                    &fgeom, 
                                    &button_layout);
    
        deco_margins.left = fgeom.borders.total.left;
        deco_margins.right = fgeom.borders.total.right;
        deco_margins.bottom = fgeom.borders.total.bottom;
        deco_margins.top = fgeom.borders.total.top;
        borders_delta = BORDERS_DELTA;
        LOGI("borders ", deco_margins.top, " ", deco_margins.bottom, " ", deco_margins.left, " ", deco_margins.right);
      
        cairo_destroy (cr);
        cairo_surface_destroy (surface);

    }
    
    void decorate_present_views ()
    {
        for (auto& view : wf::get_core().get_all_views()) {
            if(view->get_title() == "nil")
                continue;
            update_view_decoration(view);
        }
    }
    
    void remove_all_decorations ()
    {
        for (auto view : wf::get_core().get_all_views()) 
        {
            if (auto toplevel = wf::toplevel_cast(view)) 
            {
                remove_decoration(toplevel);
                wf::get_core().tx_manager->schedule_object(toplevel->toplevel());
            }
        }
    }
    
    bool ignore_decoration_of_view(wayfire_view view) 
    {
        return ignore_views.matches(view);
    }

    bool should_decorate_view(wayfire_toplevel_view view) 
    {
        return view->should_be_decorated() && !ignore_decoration_of_view(view);
    }

    void adjust_new_decorations(wayfire_toplevel_view view) 
    {
        auto toplevel = view->toplevel();

        toplevel->store_data(std::make_unique<simple_decorator_t>(view));
        auto  deco    = toplevel->get_data<simple_decorator_t>();
        auto& pending = toplevel->pending();
        pending.margins = deco->get_margins(pending);

        if (!pending.fullscreen && !pending.tiled_edges) {
            pending.geometry = wf::expand_geometry_by_margins(pending.geometry, pending.margins);
            if (view->get_output())
            {
                pending.geometry = wf::clamp(pending.geometry, view->get_output()->workarea->get_workarea());
            }
        }
    }

    void remove_decoration(wayfire_toplevel_view view) {
        view->toplevel()->erase_data<simple_decorator_t>();
        auto& pending = view->toplevel()->pending();
        if (!pending.fullscreen && !pending.tiled_edges) {
            pending.geometry = wf::shrink_geometry_by_margins(pending.geometry, pending.margins);
        }
        LOGI("remove_decoration");
        pending.margins = {0, 0, 0, 0};
    }

    void update_view_decoration(wayfire_view view) {
        if (auto toplevel = wf::toplevel_cast(view)) {
            if (should_decorate_view(toplevel)) {
                adjust_new_decorations(toplevel);
            } else {
                remove_decoration(toplevel);
            }

            wf::get_core().tx_manager->schedule_object(toplevel->toplevel());
        }
    }
    
    wf::signal::connection_t<wf::reload_config_signal> on_config_reload;
public:
    
    void init() override {
        LOGI("metacity_decor init");
        setup();
        decorate_present_views ();
        on_config_reload = [=] (auto)
        {
            if (config_changed)
            {
                remove_all_decorations ();
                setup();
                decorate_present_views ();
            }                
            config_changed = false;
        };
        wf::get_core().connect(&on_config_reload);
        theme.set_callback([=] () { config_changed = true; });
        font.set_callback([=] () { config_changed = true; });
        buttons.set_callback([=] () { config_changed = true; });
        dialog_buttons.set_callback([=] () { config_changed = true; });
        text_color.set_callback([=] () { config_changed = true; });
        background_color.set_callback([=] () { config_changed = true; });
        
        wf::get_core().connect(&on_decoration_state_updated);
        wf::get_core().tx_manager->connect(&on_new_tx);

    }

    void fini() override 
    {
        LOGI("metacity_decor stopped");
        remove_all_decorations ();
    }

};

DECLARE_WAYFIRE_PLUGIN(wayfire_metacity_decor_t);
