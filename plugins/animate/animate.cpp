#include <wayfire/singleton-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/workspace-manager.hpp>
#include <type_traits>
#include <map>
#include <wayfire/core.hpp>
#include "system_fade.hpp"
#include "basic_animations.hpp"
#include "fire/fire.hpp"
#include <wayfire/matcher.hpp>

void animation_base::init(wayfire_view, int, wf_animation_type)
{}
bool animation_base::step()
{
    return false;
}

void animation_base::reverse()
{}

int animation_base::get_direction()
{
    return 1;
}

animation_base::~animation_base()
{}

static constexpr const char *animate_custom_data_fire     = "animation-hook-fire";
static constexpr const char *animate_custom_data_zoom     = "animation-hook-zoom";
static constexpr const char *animate_custom_data_fade     = "animation-hook-fade";
static constexpr const char *animate_custom_data_minimize =
    "animation-hook-minimize";

static constexpr int HIDDEN = 0;
static constexpr int SHOWN  = 1;

/* Represents an animation running for a specific view
 * animation_t is which animation to use (i.e fire, zoom, etc). */
struct animation_hook_base : public wf::custom_data_t
{
    virtual void stop_hook(bool) = 0;
    virtual void reverse(wf_animation_type) = 0;
    virtual int get_direction() = 0;

    animation_hook_base() = default;
    virtual ~animation_hook_base() = default;
    animation_hook_base(const animation_hook_base &) = default;
    animation_hook_base(animation_hook_base &&) = default;
    animation_hook_base& operator =(const animation_hook_base&) = default;
    animation_hook_base& operator =(animation_hook_base&&) = default;
};

template<class animation_t>
struct animation_hook : public animation_hook_base
{
    static_assert(std::is_base_of<animation_base, animation_t>::value,
        "animation_type must be derived from animation_base!");

    wf_animation_type type;
    wayfire_view view;
    std::string name;
    wf::output_t *current_output = nullptr;
    std::unique_ptr<animation_base> animation;

    /* Update animation right before each frame */
    wf::effect_hook_t update_animation_hook = [=] ()
    {
        view->damage();
        bool result = animation->step();
        view->damage();

        if (!result)
        {
            stop_hook(false);
        }
    };

    /**
     * Switch the output the view is being animated on, and update the lastly
     * animated output in the global list.
     */
    void set_output(wf::output_t *new_output)
    {
        if (current_output)
        {
            current_output->render->rem_effect(&update_animation_hook);
        }

        if (new_output)
        {
            new_output->render->add_effect(&update_animation_hook,
                wf::OUTPUT_EFFECT_PRE);
        }

        current_output = new_output;
    }

    wf::signal_connection_t on_set_output = {[this] (wf::signal_data_t *data)
        { set_output(view->get_output()); }
    };

    animation_hook(wayfire_view view, int duration, wf_animation_type type,
        std::string name)
    {
        this->type = type;
        this->view = view;
        this->name = name;

        animation = std::make_unique<animation_t>();
        animation->init(view, duration, type);

        set_output(view->get_output());
        /* Animation is driven by the output render cycle the view is on.
         * Thus, we need to keep in sync with the current output. */
        view->connect_signal("set-output", &on_set_output);
    }

    void stop_hook(bool detached) override
    {
        /* We don't want to change the state of the view if it was detached */
        if ((type == ANIMATION_TYPE_MINIMIZE) && !detached)
        {
            view->set_minimized(true);
        }

        view->erase_data(name);
    }

    void reverse(wf_animation_type type) override
    {
        this->type = type;
        if (animation)
        {
            animation->reverse();
        }
    }

    int get_direction() override
    {
        return animation->get_direction();
    }

    ~animation_hook()
    {
        /**
         * Order here is very important.
         * After doing unref() the view will be potentially destroyed.
         * Hence, we want to deinitialize everything else before that.
         */
        set_output(nullptr);
        on_set_output.disconnect();
        this->animation.reset();

        // remove from list
        if (type == ANIMATION_TYPE_UNMAP)
        {
            view->unref();
        }
    }

    animation_hook(const animation_hook &) = delete;
    animation_hook(animation_hook &&) = delete;
    animation_hook& operator =(const animation_hook&) = delete;
    animation_hook& operator =(animation_hook&&) = delete;
};

static void cleanup_views_on_output(wf::output_t *output)
{
    for (auto& view : wf::get_core().get_all_views())
    {
        auto wo = view->get_output();
        if ((wo != output) && output)
        {
            continue;
        }

        if (view->has_data(animate_custom_data_fire))
        {
            view->get_data<animation_hook_base>(
                animate_custom_data_fire)->stop_hook(true);
        }

        if (view->has_data(animate_custom_data_zoom))
        {
            view->get_data<animation_hook_base>(
                animate_custom_data_zoom)->stop_hook(true);
        }

        if (view->has_data(animate_custom_data_fade))
        {
            view->get_data<animation_hook_base>(
                animate_custom_data_fade)->stop_hook(true);
        }

        if (view->has_data(animate_custom_data_minimize))
        {
            view->get_data<animation_hook_base>(
                animate_custom_data_minimize)->stop_hook(true);
        }
    }
}

/**
 * Cleanup when the last animate plugin is unloaded.
 */
struct animation_global_cleanup_t
{
    animation_global_cleanup_t() = default;
    ~animation_global_cleanup_t()
    {
        cleanup_views_on_output(nullptr);
    }

    animation_global_cleanup_t(const animation_global_cleanup_t &) = delete;
    animation_global_cleanup_t(animation_global_cleanup_t &&) = delete;
    animation_global_cleanup_t& operator =(const animation_global_cleanup_t&) =
    delete;
    animation_global_cleanup_t& operator =(animation_global_cleanup_t&&) = delete;
};

class wayfire_animation : public wf::singleton_plugin_t<animation_global_cleanup_t,
        true>
{
    wf::option_wrapper_t<std::string> open_animation{"animate/open_animation"};
    wf::option_wrapper_t<std::string> close_animation{"animate/close_animation"};

    wf::option_wrapper_t<int> default_duration{"animate/duration"};
    wf::option_wrapper_t<int> fade_duration{"animate/fade_duration"};
    wf::option_wrapper_t<int> zoom_duration{"animate/zoom_duration"};
    wf::option_wrapper_t<int> fire_duration{"animate/fire_duration"};

    wf::option_wrapper_t<int> startup_duration{"animate/startup_duration"};

    wf::view_matcher_t animation_enabled_for{"animate/enabled_for"};
    wf::view_matcher_t fade_enabled_for{"animate/fade_enabled_for"};
    wf::view_matcher_t zoom_enabled_for{"animate/zoom_enabled_for"};
    wf::view_matcher_t fire_enabled_for{"animate/fire_enabled_for"};

  public:
    void init() override
    {
        singleton_plugin_t::init();

        grab_interface->name = "animate";
        grab_interface->capabilities = 0;

        output->connect_signal("view-mapped", &on_view_mapped);
        output->connect_signal("view-pre-unmapped", &on_view_unmapped);
        output->connect_signal("start-rendering", &on_render_start);
        output->connect_signal("view-minimize-request", &on_minimize_request);
    }

    struct view_animation_t
    {
        std::string animation_name;
        int duration;
    };

    view_animation_t get_animation_for_view(
        wf::option_wrapper_t<std::string>& anim_type, wayfire_view view)
    {
        /* Determine the animation for the given view.
         * Note that the matcher plugin might not have been loaded, so
         * we need to have a fallback algorithm */
        if (fade_enabled_for.matches(view))
        {
            return {"fade", fade_duration};
        }

        if (zoom_enabled_for.matches(view))
        {
            return {"zoom", zoom_duration};
        }

        if (fire_enabled_for.matches(view))
        {
            return {"fire", fire_duration};
        }

        if (animation_enabled_for.matches(view))
        {
            return {anim_type, default_duration};
        }

        return {"none", 0};
    }

    bool try_reverse(wayfire_view view, wf_animation_type type, std::string name,
        int visibility)
    {
        visibility = !visibility;
        if (view->has_data(name))
        {
            auto data = view->get_data<animation_hook_base>(name);
            if (data->get_direction() == visibility)
            {
                data->reverse(type);
                return true;
            }
        }

        return false;
    }

    template<class animation_t>
    void set_animation(wayfire_view view,
        wf_animation_type type, int duration, std::string name)
    {
        name = "animation-hook-" + name;

        if (type == ANIMATION_TYPE_UNMAP)
        {
            view->take_ref();
            view->take_snapshot();
        }

        if (type == ANIMATION_TYPE_MAP)
        {
            if (try_reverse(view, type, name, SHOWN))
            {
                return;
            }

            auto animation = get_animation_for_view(open_animation, view);
            view->store_data(
                std::make_unique<animation_hook<animation_t>>(view, duration, type,
                    name), name);
        } else if (type == ANIMATION_TYPE_UNMAP)
        {
            if (try_reverse(view, type, name, HIDDEN))
            {
                return;
            }

            auto animation = get_animation_for_view(close_animation, view);
            view->store_data(
                std::make_unique<animation_hook<animation_t>>(view, duration, type,
                    name), name);
        } else if (type & MINIMIZE_STATE_ANIMATION)
        {
            if (view->has_data(animate_custom_data_minimize))
            {
                view->get_data<animation_hook_base>(
                    animate_custom_data_minimize)->reverse(type);
                return;
            }

            view->store_data(
                std::make_unique<animation_hook<animation_t>>(view, duration, type,
                    animate_custom_data_minimize),
                animate_custom_data_minimize);
        }
    }

    /* TODO: enhance - add more animations */
    wf::signal_connection_t on_view_mapped =
        [=] (wf::signal_data_t *ddata) -> void
    {
        auto view = get_signaled_view(ddata);
        auto animation = get_animation_for_view(open_animation, view);

        if (animation.animation_name == "fade")
        {
            set_animation<fade_animation>(view, ANIMATION_TYPE_MAP,
                animation.duration, animation.animation_name);
        } else if (animation.animation_name == "zoom")
        {
            set_animation<zoom_animation>(view, ANIMATION_TYPE_MAP,
                animation.duration, animation.animation_name);
        } else if (animation.animation_name == "fire")
        {
            set_animation<FireAnimation>(view, ANIMATION_TYPE_MAP,
                animation.duration, animation.animation_name);
        }
    };

    wf::signal_connection_t on_view_unmapped = [=] (wf::signal_data_t *data)
    {
        auto view = get_signaled_view(data);
        auto animation = get_animation_for_view(close_animation, view);

        if (animation.animation_name == "fade")
        {
            set_animation<fade_animation>(view, ANIMATION_TYPE_UNMAP,
                animation.duration, animation.animation_name);
        } else if (animation.animation_name == "zoom")
        {
            set_animation<zoom_animation>(view, ANIMATION_TYPE_UNMAP,
                animation.duration, animation.animation_name);
        } else if (animation.animation_name == "fire")
        {
            set_animation<FireAnimation>(view, ANIMATION_TYPE_UNMAP,
                animation.duration, animation.animation_name);
        }
    };

    wf::signal_connection_t on_minimize_request = [=] (wf::signal_data_t *data)
    {
        auto ev = static_cast<wf::view_minimize_request_signal*>(data);
        if (ev->state)
        {
            ev->carried_out = true;
            set_animation<zoom_animation>(
                ev->view, ANIMATION_TYPE_MINIMIZE, default_duration, "minimize");
        } else
        {
            set_animation<zoom_animation>(
                ev->view, ANIMATION_TYPE_RESTORE, default_duration, "minimize");
        }
    };

    wf::signal_connection_t on_render_start = [=] (wf::signal_data_t *data)
    {
        new wf_system_fade(output, startup_duration);
    };

    void fini() override
    {
        /* Clear up all active animations on the current output */
        cleanup_views_on_output(output);
        singleton_plugin_t::fini();
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_animation);
