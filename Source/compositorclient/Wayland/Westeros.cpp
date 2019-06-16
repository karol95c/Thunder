#include "Module.h"

#define EGL_EGLEXT_PROTOTYPES 1

#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifdef BCM_HOST
#include <bcm_host.h>
#endif

#include "../Client.h"
#include "Implementation.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <simpleshell-client-protocol.h>
#include <wayland-client-core.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <unistd.h>

// logical xor
#define XOR(a, b) ((!a && b) || (a && !b))

using namespace WPEFramework;

#define Trace(fmt, args...) fprintf(stderr, "[pid=%d][Client %s:%d] : " fmt, getpid(), __FILE__, __LINE__, ##args)

#define RED_SIZE (8)
#define GREEN_SIZE (8)
#define BLUE_SIZE (8)
#define ALPHA_SIZE (8)
#define DEPTH_SIZE (0)

static const struct wl_shell_surface_listener g_ShellSurfaceListener = {
    //handle_ping,
    [](void* data, struct wl_shell_surface* shell_surface, uint32_t serial) {
        wl_shell_surface_pong(shell_surface, serial);
    },
    //handle_configure,
    [](void* data, struct wl_shell_surface* shell_surface, uint32_t edges, int32_t width, int32_t height) {
        Trace("handle_configure: width=%d height=%d \n", width, height);
        //  Wayland::Display::Sur *wayland = static_cast<Wayland *>(data);
        // wl_egl_window_resize(wayland->eglWindow, width, height, 0, 0);
    },
    //handle_popup_done
    [](void* data, struct wl_shell_surface* shell_surface) {
    }
};

struct wl_shm_listener shmListener = {
    // shmFormat
    [](void* data, struct wl_shm* wl_shm, uint32_t format) {
        Trace("shm format: %X\n", format);
    }
};

static const struct wl_output_listener outputListener = {
    // outputGeometry
    [](void* data, struct wl_output* output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make, const char* model, int32_t transform) {
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));
        Trace("wl_output_listener.outputGeometry x=%d y=%d physical_width=%d physical_height=%d, make=%s: model=%s transform=%d subpixel%d\n",
            x, y, physical_width, physical_height, make, model, transform, subpixel);
        Wayland::Display::Rectangle& rect(const_cast<Wayland::Display::Rectangle&>(context.Physical()));
        rect.X = x;
        rect.Y = y;
        rect.Width = physical_width;
        rect.Height = physical_height;
    },
    // outputMode
    [](void* data, struct wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));

        const Wayland::Display::Rectangle& rect(context.Physical());

        if ((flags & WL_OUTPUT_MODE_CURRENT) && ((width != rect.Width) || (height != rect.Height))) {
            Wayland::Display::Rectangle& rect(const_cast<Wayland::Display::Rectangle&>(context.Physical()));
            rect.Width = width;
            rect.Height = height;
            Trace("wl_output_listener.outputMode [0,0,%d,%d]\n", width, height);
        }
    },
    // outputDone
    [](void* data, struct wl_output* output) {
    },
    // outputScale
    [](void* data, struct wl_output* output, int32_t factor) {
    }
};

static const struct wl_keyboard_listener keyboardListener = {
    // keyboardKeymap
    [](void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size) {
        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
            close(fd);
        } else {
            void* mapping = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
            if (mapping == MAP_FAILED) {
                close(fd);
            } else {

                Wayland::Display& context = *(static_cast<Wayland::Display*>(data));
                context.KeyMapConfiguration(static_cast<const char*>(mapping), size);
                munmap(mapping, size);
                close(fd);
            }
        }
        Trace("wl_keyboard_listener.keyboardKeymap [%d,%d]\n", format, size);
    },
    // keyboardEnter,
    [](void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface, struct wl_array* keys) {
        Trace("wl_keyboard_listener.keyboardEnter serial=%d\n", serial);
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));
        context.FocusKeyboard(surface, true);
    },
    // keyboardLeave,
    [](void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface) {
        Trace("wl_keyboard_listener.keyboardLeave serial=%d\n", serial);
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));
        context.FocusKeyboard(surface, false);
    },
    // keyboardKey
    [](void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));

        // Have no idea if this is true, just lets see...
        assert(keyboard == context._keyboard);

        Wayland::Display::IKeyboard::state action;
        switch (state) {
        case WL_KEYBOARD_KEY_STATE_RELEASED:
            action = Wayland::Display::IKeyboard::released;
            break;
        case WL_KEYBOARD_KEY_STATE_PRESSED:
            action = Wayland::Display::IKeyboard::pressed;
            break;
        default:
            action = static_cast<Wayland::Display::IKeyboard::state>(state);
        }
        context.Key(key, action, time);

        Trace("wl_keyboard_listener.keyboardKey [0x%02X, %s, 0x%02X ]\n", key, state == WL_KEYBOARD_KEY_STATE_PRESSED ? "Pressed" : "Released", context._keyModifiers);
    },
    // keyboardModifiers
    [](void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
        Trace("wl_keyboard_listener.keyboardModifiers [%d,%d,%d]\n", mods_depressed, mods_latched, mods_locked);
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));
        context.Modifiers(mods_depressed, mods_latched, mods_locked, group);
    },
    // keyboardRepeatInfo
    [](void* data, struct wl_keyboard* keyboard, int32_t rate, int32_t delay) {
        Trace("wl_keyboard_listener.keyboardRepeatInfo [%d,%d]\n", rate, delay);
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));
        context.Repeat(rate, delay);
    }
};

static const struct wl_pointer_listener pointerListener = {
    // pointerEnter
    [](void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface, wl_fixed_t sx, wl_fixed_t sy) {
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));
        int x, y;

        x = wl_fixed_to_int(sx);
        y = wl_fixed_to_int(sy);

        Trace("wl_pointer_listener.pointerEnter [%d,%d]\n", x, y);
        context.FocusPointer(surface, true);
    },
    // pointerLeave
    [](void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface) {
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));
        Trace("wl_pointer_listener.pointerLeave [%p]\n", surface);
        context.FocusPointer(surface, false);
    },
    // pointerMotion
    [](void* data, struct wl_pointer* pointer, uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
        int x, y;

        x = wl_fixed_to_int(sx);
        y = wl_fixed_to_int(sy);

        Trace("wl_pointer_listener.pointerMotion [%d,%d]\n", x, y);
    },
    // pointerButton
    [](void* data, struct wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
        Trace("wl_pointer_listener.pointerButton [%u,%u]\n", button, state);
    },
    // pointerAxis
    [](void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
        int v;
        v = wl_fixed_to_int(value);
        Trace("wl_pointer_listener.pointerAxis [%u,%d]\n", axis, v);
    }
};

static const struct wl_seat_listener seatListener = {
    // seatCapabilities,
    [](void* data, struct wl_seat* seat, uint32_t capabilities) {
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));

        Trace("wl_seat_listener.seatCapabilities [%p,%d]\n", seat, capabilities);

        if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
            context._keyboard = wl_seat_get_keyboard(context._seat);
            wl_keyboard_add_listener(context._keyboard, &keyboardListener, data);
            Trace("wl_seat_listener.keyboard [%p,%p]\n", seat, context._keyboard);
        }
        if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
            context._pointer = wl_seat_get_pointer(context._seat);
            wl_pointer_add_listener(context._pointer, &pointerListener, data);
            Trace("wl_seat_listener.pointer [%p,%p]\n", seat, context._pointer);
        }
        if (capabilities & WL_SEAT_CAPABILITY_TOUCH) {
            context._touch = wl_seat_get_touch(context._seat);
            Trace("wl_seat_listener.touch [%p,%p]\n", seat, context._touch);
        }
    },
    // seatName
    [](void* data, struct wl_seat* seat, const char* name) {
        Trace("wl_seat_listener.seatName[%p,%s]\n", seat, name);
    }
};

static const struct wl_simple_shell_listener simpleShellListener = {
    // surface_id
    [](void* data, struct wl_simple_shell* shell, struct wl_surface* surface, uint32_t surfaceId) {
        Trace("wl_simple_shell_listener.surface_id shell=%p wl_surface=%p surfaceId=%d\n", shell, surface, surfaceId);
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));

        // Have no idea if this is true, just lets see...
        assert(shell == context._simpleShell);
        context.Constructed(surfaceId, surface);

        Wayland::Display::Surface waylandSurface;
        context.Get(surfaceId, waylandSurface);

        if ((waylandSurface.IsValid() == true) && (waylandSurface.UpScale() == true)) {
            Trace("wl_simple_shell_listener.surface_id upscaling %s to (%dx%d)\n",
                waylandSurface.Name().c_str(), context.Physical().Width, context.Physical().Height);

            wl_simple_shell_set_geometry(shell, surfaceId, context.Physical().X, context.Physical().Y,
                context.Physical().Width, context.Physical().Height);
        }
    },
    // surface_created
    [](void* data, struct wl_simple_shell* shell, uint32_t surfaceId, const char* name) {
        Trace("wl_simple_shell_listener.surface_created shell=%p name=%s surfaceId=%d\n", shell, name, surfaceId);
    },
    // surface_destroyed
    [](void* data, struct wl_simple_shell* shell, uint32_t surfaceId, const char* name) {
        Trace("wl_simple_shell_listener.surface_destroyed shell=%p name=%s surfaceId=%d\n", shell, name, surfaceId);
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));

        // Have no idea if this is true, just lets see...
        assert(shell == context._simpleShell);

        context.Destructed(surfaceId);
        Trace("wl_simple_shell_listener.surface_destroyed surfaceId=%d\n", surfaceId);
    },
    // surface_status
    [](void* data, struct wl_simple_shell* shell, uint32_t surfaceId, const char* name, uint32_t visible,
        int32_t x, int32_t y, int32_t width, int32_t height, wl_fixed_t opacity, wl_fixed_t zorder) {
        Trace("surface_status surfaceId=%d name=%s width=%d  height=%d x=%d, y=%d visible=%d opacity=%d zorder=%d\n", surfaceId, name, width, height, x, y, visible, opacity, zorder);
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));

        // Have no idea if this is true, just lets see...
        assert(shell == context._simpleShell);

        context.Dimensions(surfaceId, visible, x, y, width, height, opacity, zorder);

        context.Constructed(surfaceId, name);
        Trace("wl_simple_shell_listener.surface_status surfaceId=%d\n", surfaceId);
    },
    // get_surfaces_done
    [](void* data, struct wl_simple_shell* shell) {
        Trace("wl_simple_shell_listener.get_surfaces_done shell=%p\n", shell);
    }
};

static const struct wl_registry_listener globalRegistryListener = {

    // global
    [](void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
        Trace("wl_registry_listener.global interface=%s name=%d version=%d\n", interface, name, version);

        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));

        if (::strcmp(interface, "wl_compositor") == 0) {
            // I expect that a compositor is tied to a display, so expect the name here to be the one of the display.
            // Lets check :-)
            context._compositor = static_cast<struct wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
        } else if (::strcmp(interface, "wl_simple_shell") == 0) {
            // A shell, is probably associated with a client, so I guess we now need to find a client..
            struct wl_simple_shell* result = static_cast<struct wl_simple_shell*>(wl_registry_bind(registry, name, &wl_simple_shell_interface, 1));
            wl_simple_shell_add_listener(result, &simpleShellListener, data);

            // This will probably trigger the return of a list of all available services.
            wl_simple_shell_get_surfaces(result);

            context._simpleShell = result;

            if (context.Collect() == true) {
                // As we are subscribed, get all surfaces we have..
                wl_simple_shell_get_surfaces(result);
            }
        } else if (::strcmp(interface, "wl_seat") == 0) {
            // A shell, is probably associated with a client, so I guess we now need to find a client..
            struct wl_seat* result = static_cast<struct wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 4));
            wl_seat_add_listener(result, &seatListener, data);
            context._seat = result;
        } else if (::strcmp(interface, "wl_shell") == 0) {
            // A shell, is probably associated with a client, so I guess we now need to find a client..
            context._shell = static_cast<struct wl_shell*>(wl_registry_bind(registry, name, &wl_shell_interface, 1));
        } else if (::strcmp(interface, "wl_output") == 0) {
            struct wl_output* result = static_cast<struct wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 2));
            wl_output_add_listener(result, &outputListener, data);
            context._output = result;
        }
    },
    // global_remove
    [](void*, struct wl_registry*, uint32_t) {
        Trace("wl_registry_listener.global_remove\n");
    },
};

namespace WPEFramework {

namespace Wayland {
    /*static*/ Display::CriticalSection Display::_adminLock;
    /*static*/ std::string Display::_runtimeDir;
    /*static*/ Display::DisplayMap Display::_displays;
    /*static*/ Display::WaylandSurfaceMap Display::_waylandSurfaces;

    static void printEGLConfiguration(EGLDisplay dpy, EGLConfig config)
    {
#define X(VAL)    \
    {             \
        VAL, #VAL \
    }
        struct {
            EGLint attribute;
            const char* name;
        } names[] = {
            X(EGL_BUFFER_SIZE),
            X(EGL_RED_SIZE),
            X(EGL_GREEN_SIZE),
            X(EGL_BLUE_SIZE),
            X(EGL_ALPHA_SIZE),
            X(EGL_CONFIG_CAVEAT),
            X(EGL_CONFIG_ID),
            X(EGL_DEPTH_SIZE),
            X(EGL_LEVEL),
            X(EGL_MAX_PBUFFER_WIDTH),
            X(EGL_MAX_PBUFFER_HEIGHT),
            X(EGL_MAX_PBUFFER_PIXELS),
            X(EGL_NATIVE_RENDERABLE),
            X(EGL_NATIVE_VISUAL_ID),
            X(EGL_NATIVE_VISUAL_TYPE),
            X(EGL_SAMPLE_BUFFERS),
            X(EGL_SAMPLES),
            X(EGL_SURFACE_TYPE),
            X(EGL_TRANSPARENT_TYPE),
        };
#undef X

        Trace("Config details:\n");
        for (unsigned int j = 0; j < sizeof(names) / sizeof(names[0]); j++) {
            EGLint value = -1;
            EGLBoolean res = eglGetConfigAttrib(dpy, config, names[j].attribute, &value);
            if (res) {
                Trace("  - %s: %d (0x%x)\n", names[j].name, value, value);
            }
        }
    }

    Display::SurfaceImplementation::SurfaceImplementation(Display& display, const std::string& name, const uint32_t width, const uint32_t height)
        : _surface(nullptr)
        , _refcount(1)
        , _level(0)
        , _name(name)
        , _id(0)
        , _x(0)
        , _y(0)
        , _width(width)
        , _height(height)
        , _visible(0)
        , _opacity(0)
        , _ZOrder(0)
        , _display(&display)
        , _native(nullptr)
        , _frameCallback(nullptr)
        , _eglSurfaceWindow(EGL_NO_SURFACE)
        , _keyboard(nullptr)
        , _pointer(nullptr)
        , _upScale(false)
    {
        assert(display.IsOperational());

        _level = 0;

        _surface = wl_compositor_create_surface(display._compositor);

        if (_surface != nullptr) {

            struct wl_region* region;
            region = wl_compositor_create_region(display._compositor);

            wl_region_add(region, 0, 0, width, height);

            // Found in WPEwayland implementation:
            wl_surface_set_opaque_region(_surface, nullptr);

            wl_region_destroy(region);

            Trace("Creating a surface of size: %d x %d\n", width, height);

            _native = wl_egl_window_create(_surface, width, height);

            assert(EGL_NO_SURFACE != _native);

            if (_native != EGL_NO_SURFACE) {

                _shellSurface = wl_shell_get_shell_surface(display._shell, _surface);

                wl_shell_surface_add_listener(_shellSurface, &g_ShellSurfaceListener, this);
            }

            if (_display->HasEGLContext() == true) {
                Connect(EGLSurface(EGL_NO_SURFACE));
            }
        }
    }

    Display::SurfaceImplementation::SurfaceImplementation(Display& display, const uint32_t id, struct wl_surface* surface)
        : _surface(surface)
        , _refcount(1)
        , _level(2)
        , _name()
        , _id(id)
        , _width(0)
        , _height(0)
        , _visible(0)
        , _opacity(0)
        , _ZOrder(0)
        , _display(&display)
        , _native(nullptr)
        , _frameCallback(nullptr)
        , _shellSurface(nullptr)
        , _eglSurfaceWindow(EGL_NO_SURFACE)
        , _keyboard(nullptr)
        , _pointer(nullptr)
        , _upScale(false)
    {
    }

    Display::SurfaceImplementation::SurfaceImplementation(Display& display, const uint32_t id, const char* name)
        : _surface(nullptr)
        , _refcount(1)
        , _level(2)
        , _name(name)
        , _id(id)
        , _width(0)
        , _height(0)
        , _visible(0)
        , _opacity(0)
        , _ZOrder(0)
        , _display(&display)
        , _native(nullptr)
        , _frameCallback(nullptr)
        , _shellSurface(nullptr)
        , _eglSurfaceWindow(EGL_NO_SURFACE)
        , _keyboard(nullptr)
        , _pointer(nullptr)
        , _upScale(false)
    {
    }

    void Display::SurfaceImplementation::Callback(wl_callback_listener* listener, void* data)
    {

        assert((listener == nullptr) ^ (_frameCallback == nullptr));

        if (listener != nullptr) {

            _frameCallback = wl_surface_frame(_surface);
            wl_callback_add_listener(_frameCallback, listener, data);

            eglSwapBuffers(_display->_eglDisplay, _eglSurfaceWindow);
        } else {
            wl_callback_destroy(_frameCallback);
            _frameCallback = nullptr;
        }
    }

    void Display::SurfaceImplementation::Resize(const int dx, const int dy, const int width, const int height)
    {
        Trace("SimpleShell Geometry  %s to h%dpx x w%dpx at position x=%d y=%d\n", _name.c_str(), width, height, dy, dy);
        wl_simple_shell_set_geometry(_display->_simpleShell, _id, dx, dy, width, height);

        Redraw();

        _width = width;
        _height = height;
        _x = dx;
        _y = dy;
    }

    void Display::SurfaceImplementation::Visibility(const bool visible)
    {
        wl_simple_shell_set_visible(_display->_simpleShell, _id, (visible ? 1 : 0));
        wl_display_flush(_display->_display);
        Redraw();
    }

    void Display::SurfaceImplementation::Opacity(const uint32_t opacity)
    {
        wl_simple_shell_set_opacity(_display->_simpleShell, _id, opacity);
        wl_display_flush(_display->_display);
        Redraw();
    }

    void Display::SurfaceImplementation::ZOrder(const uint32_t order)
    {
        wl_simple_shell_set_zorder(_display->_simpleShell, _id, order);
        wl_display_flush(_display->_display);
        Redraw();
    }

    void Display::SurfaceImplementation::BringToFront()
    {
        wl_shell_surface_set_toplevel(_shellSurface);
        wl_display_flush(_display->_display);
        Redraw();
    }

    void Display::SurfaceImplementation::SetTop()
    {
        wl_simple_shell_set_zorder(_display->_simpleShell, _id, 0);
        wl_display_flush(_display->_display);
        Redraw();
    }

    void Display::SurfaceImplementation::Dimensions(
        const uint32_t visible,
        const int32_t x, const int32_t y, const int32_t width, const int32_t height,
        const uint32_t opacity,
        const uint32_t zorder)
    {
        Trace("Updated surfaceId=%d width=%d  height=%d x=%d, y=%d visible=%d opacity=%d zorder=%d\n", _id, width, height, x, y, visible, opacity, zorder);

        _visible = visible;
        _opacity = opacity;
        _ZOrder = zorder;
        // This is the response form the status, but if we created the window, we need to check
        // and set according to the request.
        if (_native != nullptr) {
            if ((_width != width) || (_height != height) || (_x != x) || (_y != y)) {
                Trace("Resizing surface %d from [%d x %d] to [%d x %d]\n", _id, _width, _height, width, height);
                wl_egl_window_resize(_native, _width, _height, x, y);
            }
        } else {
            // Update this surface
            Trace("Update surface %d from [%d x %d] to [%d x %d]\n", _id, _width, _height, width, height);
            _x = x;
            _y = y;
            _width = width;
            _height = height;
        }

        wl_display_flush(_display->_display);

        Trace("Current surfaceId=%d width=%d  height=%d x=%d, y=%d, visible=%d opacity=%d zorder=%d\n", _id, _width, _height, _x, _y, _visible, _opacity, _ZOrder);
    }

    void Display::SurfaceImplementation::Redraw()
    {
        _display->Trigger();

        // wait for wayland to flush events
        sem_wait(&(_display->_redraw));
        if (_native != nullptr) {
            eglSwapBuffers(_display->_eglDisplay, _eglSurfaceWindow);
        }
    }

    void Display::SurfaceImplementation::Unlink()
    {
        if (_display != nullptr) {

            if (_frameCallback != nullptr) {
                wl_callback_destroy(_frameCallback);
            }

            if (_eglSurfaceWindow != EGL_NO_SURFACE) {

                eglDestroySurface(_display->_eglDisplay, _eglSurfaceWindow);
                _eglSurfaceWindow = EGL_NO_SURFACE;
            }

            if (_native != nullptr) {
                wl_egl_window_destroy(_native);
                _native = nullptr;
            }

            if (_shellSurface != nullptr) {
                wl_shell_surface_destroy(_shellSurface);
                _shellSurface = nullptr;
            }

            if (_surface != nullptr) {
                wl_surface_destroy(_surface);
                _surface = nullptr;
            }

            _display = nullptr;
        }
    }

    bool Display::SurfaceImplementation::Connect(const EGLSurface& surface)
    {
        if (surface != EGL_NO_SURFACE) {
            _eglSurfaceWindow = surface;
        } else {
            if (_display->_eglContext == EGL_NO_CONTEXT) {
                _display->InitializeEGL();
            }

            if (_display->_eglContext != EGL_NO_CONTEXT) {
                /*
                 * Create a window surface
                 */
                _eglSurfaceWindow = eglCreateWindowSurface(
                    _display->_eglDisplay,
                    _display->_eglConfig,
                    reinterpret_cast<EGLNativeWindowType>(_native),
                    nullptr);

                if (_eglSurfaceWindow == EGL_NO_SURFACE) {

                    _eglSurfaceWindow = eglCreateWindowSurface(
                        _display->_eglDisplay,
                        _display->_eglConfig,
                        static_cast<EGLNativeWindowType>(nullptr),
                        nullptr);
                }

                assert(EGL_NO_SURFACE != _eglSurfaceWindow);

                EGLint height(0);
                EGLint width(0);
                eglQuerySurface(_display->_eglDisplay, _eglSurfaceWindow, EGL_WIDTH, &width);
                eglQuerySurface(_display->_eglDisplay, _eglSurfaceWindow, EGL_HEIGHT, &height);

                Trace("EGL window surface is %dx%d\n", height, width);
            }
        }

        if (_eglSurfaceWindow != EGL_NO_SURFACE) {
            /*
             * Establish EGL context for this thread
             */

            int result = eglMakeCurrent(_display->_eglDisplay, _eglSurfaceWindow, _eglSurfaceWindow, _display->_eglContext);
            assert(EGL_FALSE != result);

            if (EGL_FALSE != result) {
                result = eglSwapInterval(_display->_eglDisplay, 1);
                assert(EGL_FALSE != result);
            }
        }

        return (_eglSurfaceWindow != EGL_NO_SURFACE);
    }

    Display::ImageImplementation::ImageImplementation(Display& display, const uint32_t texture, const uint32_t width, const uint32_t height)
        : _refcount(1)
        , _display(&display)
    {
        _eglImageKHR = eglCreateImageKHR(_display->_eglDisplay, _display->_eglContext, EGL_GL_TEXTURE_2D_KHR,
            reinterpret_cast<EGLClientBuffer>(texture), 0);
    }

    Display::ImageImplementation::~ImageImplementation()
    {
        if (_display != nullptr) {
            eglDestroyImageKHR(_display->_eglDisplay, _eglImageKHR);
        }
    }

    static void* Processor(void* data)
    {
        Wayland::Display& context = *(static_cast<Wayland::Display*>(data));

        while ((sem_wait(&context._trigger) == 0) && (context._display != nullptr)) {
            Trace("Flush Events\n");
            wl_display_flush(context._display);

            context.Redraw();
        }
        return nullptr;
    }
    Display::~Display()
    {
        ASSERT(_refCount == 0);
        DisplayMap::iterator index(_displays.find(_displayName));

        if (index != _displays.end()) {
            _displays.erase(index);
        }
#ifdef BCM_HOST
        bcm_host_deinit();
#endif
    }


    void Display::Initialize()
    {
        if (_display != nullptr)
            return;

        if (_runtimeDir.empty() == true) {
            const char* envName = ::getenv("XDG_RUNTIME_DIR");
            if (envName != nullptr) {
                _runtimeDir = envName;
            }
        }

        Trace("Initialize Wayland Display on %s\n", _runtimeDir.c_str());
        Trace("Initialize Wayland Display Name %s\n", _displayName.c_str());
        if (_displayId.empty() == false)
            Trace("Connecting to Wayland Display %s\n", _displayId.c_str());

        _display = wl_display_connect(_displayId.empty() == false ? _displayId.c_str() : nullptr);

        assert(_display != nullptr);

        if (_display != nullptr) {
            _registry = wl_display_get_registry(_display);

            assert(_registry != nullptr);

            if (_registry != nullptr) {

                wl_registry_add_listener(_registry, &globalRegistryListener, this);
                wl_display_roundtrip(_display);

                sem_init(&_trigger, 0, 0);
                sem_init(&_redraw, 0, 0);

                Trace("creating communication thread\n");
                if (pthread_create(&_tid, nullptr, Processor, this) != 0) {
                    Trace("[Wayland] Error creating communication thread\n");
                }
            }
        }
    }

    void Display::InitializeEGL()
    {
        /*
         * Get default EGL display
         */
        _eglDisplay = eglGetDisplay(reinterpret_cast<NativeDisplayType>(_display));

        Trace("Display: %p\n", _eglDisplay);

        if (_eglDisplay == EGL_NO_DISPLAY) {
            Trace("Oops bad Display: %p\n", _eglDisplay);
        } else {
            /*
             * Initialize display
             */
            EGLint major, minor;
            if (eglInitialize(_eglDisplay, &major, &minor) != EGL_TRUE) {
                Trace("Unable to initialize EGL: %X\n", eglGetError());
            } else {
                /*
                 * Get number of available configurations
                 */
                EGLint configCount;
                Trace("Vendor: %s\n", eglQueryString(_eglDisplay, EGL_VENDOR));
                Trace("Version: %d.%d\n", major, minor);

                if (eglGetConfigs(_eglDisplay, nullptr, 0, &configCount)) {

                    EGLConfig eglConfigs[configCount];

                    EGLint attributes[] = {
                        EGL_RED_SIZE, RED_SIZE,
                        EGL_GREEN_SIZE, GREEN_SIZE,
                        EGL_BLUE_SIZE, BLUE_SIZE,
                        EGL_DEPTH_SIZE, DEPTH_SIZE,
                        EGL_STENCIL_SIZE, 0,
                        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                        EGL_NONE
                    };

                    Trace("Configs: %d\n", configCount);
                    /*
                     * Get a list of configurations that meet or exceed our requirements
                     */
                    if (eglChooseConfig(_eglDisplay, attributes, eglConfigs, configCount, &configCount)) {

                        /*
                         * Choose a suitable configuration
                         */
                        int index = 0;

                        while (index < configCount) {
                            EGLint redSize, greenSize, blueSize, alphaSize, depthSize;

                            eglGetConfigAttrib(_eglDisplay, eglConfigs[index], EGL_RED_SIZE, &redSize);
                            eglGetConfigAttrib(_eglDisplay, eglConfigs[index], EGL_GREEN_SIZE, &greenSize);
                            eglGetConfigAttrib(_eglDisplay, eglConfigs[index], EGL_BLUE_SIZE, &blueSize);
                            eglGetConfigAttrib(_eglDisplay, eglConfigs[index], EGL_ALPHA_SIZE, &alphaSize);
                            eglGetConfigAttrib(_eglDisplay, eglConfigs[index], EGL_DEPTH_SIZE, &depthSize);

                            if ((redSize == RED_SIZE) && (greenSize == GREEN_SIZE) && (blueSize == BLUE_SIZE) && (alphaSize == ALPHA_SIZE) && (depthSize >= DEPTH_SIZE)) {
                                break;
                            }

                            index++;
                        }
                        if (index < configCount) {
                            _eglConfig = eglConfigs[index];

                            EGLint attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2 /* ES2 */, EGL_NONE };

                            Trace("Config choosen: %d\n", index);
                            printEGLConfiguration(_eglDisplay, _eglConfig);

                            /*
                             * Create an EGL context
                             */
                            _eglContext = eglCreateContext(_eglDisplay, _eglConfig, EGL_NO_CONTEXT, attributes);

                            Trace("Context created\n");
                        }
                    }
                }
                Trace("Extentions: %s\n", eglQueryString(_eglDisplay, EGL_EXTENSIONS));
            }
        }
    }

    void Display::Deinitialize()
    {
        _adminLock.Lock();

        _keyboardReceiver = nullptr;

        // First notify our client of our destruction...
        SurfaceMap::iterator index(_surfaces.begin());

        while (index != _surfaces.end()) {
            // Remove the entry from global list
            WaylandSurfaceMap::iterator entry(_waylandSurfaces.find(index->second->_surface));

            if (entry != _waylandSurfaces.end()) {
                entry->second->Release();
                _waylandSurfaces.erase(entry);
            }

            index->second->Unlink();
            index->second->Release();
            index++;
        }

        WaylandSurfaceMap::iterator entry(_waylandSurfaces.begin());

        while (entry != _waylandSurfaces.end()) {

            entry->second->Release();
            entry++;
        }

        _waylandSurfaces.clear();
        _surfaces.clear();

        if (_eglContext != EGL_NO_CONTEXT) {
            eglMakeCurrent(_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglTerminate(_eglDisplay);
            eglReleaseThread();
        }
        if (_output != nullptr) {
            wl_output_destroy(_output);
            _output = nullptr;
        }

        if (_simpleShell != nullptr) {
            wl_simple_shell_destroy(_simpleShell);
            _simpleShell = nullptr;
        }

        if (_shell != nullptr) {
            wl_shell_destroy(_shell);
            _shell = nullptr;
        }

        if (_seat != nullptr) {
            wl_seat_destroy(_seat);
            _seat = nullptr;
        }

        if (_keyboard) {
            wl_keyboard_destroy(_keyboard);
            _keyboard = nullptr;
        }

        if (_compositor != nullptr) {
            wl_compositor_destroy(_compositor);
            _compositor = nullptr;
        }

        if (_registry != nullptr) {
            wl_registry_destroy(_registry);
            _registry = nullptr;
        }
        if (_display != nullptr) {
            wl_display_disconnect(_display);
            _display = nullptr;
        }
        _adminLock.Unlock();

        Trigger();

        pthread_join(_tid, nullptr);
    }

    void Display::LoadSurfaces()
    {
        _collect |= true;

        if (_simpleShell != nullptr) {
            // As we are subscribed, get all surfaces we have..
            wl_simple_shell_get_surfaces(_simpleShell);
        }
    }

    Compositor::IDisplay::ISurface* Display::Create(const std::string& name, const uint32_t width, const uint32_t height)
    {
        IDisplay::ISurface* result = nullptr;

        _adminLock.Lock();

        SurfaceImplementation* surface = new SurfaceImplementation(*this, name, width, height);

        // Wait till we are fully registered.
        _waylandSurfaces.insert(std::pair<struct wl_surface*, SurfaceImplementation*>(surface->_surface, surface));

        result = surface;

        _adminLock.Unlock();

        return (result);
    }

    Display::Image Display::Create(const uint32_t texture, const uint32_t width, const uint32_t height)
    {
        return (Image(*new ImageImplementation(*this, texture, width, height)));
    }

    void Display::Constructed(const uint32_t id, wl_surface* surface)
    {
        _adminLock.Lock();

        WaylandSurfaceMap::iterator index = _waylandSurfaces.find(surface);

        if (index != _waylandSurfaces.end()) {
            wl_simple_shell_set_name(_simpleShell, id, index->second->Name().c_str());
            // Do not forget to update the actual surface, it is now alive..
            index->second->_id = id;
            index->second->AddRef();
            _surfaces.insert(std::pair<uint32_t, Display::SurfaceImplementation*>(id, index->second));
        } else if (_collect == true) {
            // Seems this is a surface, we did not create.
            Display::SurfaceImplementation* entry(new Display::SurfaceImplementation(*this, id, surface));
            entry->AddRef();
            _surfaces.insert(std::pair<uint32_t, Display::SurfaceImplementation*>(id, entry));
            _waylandSurfaces.insert(std::pair<wl_surface*, Display::SurfaceImplementation*>(surface, entry));
        }

        if (_clientHandler != nullptr) {
            _clientHandler->Attached(id);
        }

        _adminLock.Unlock();
    }

    void Display::Constructed(const uint32_t id, const char* name)
    {
        _adminLock.Lock();

        SurfaceMap::iterator index = _surfaces.find(id);

        if (index != _surfaces.end()) {
            index->second->Name(name);
        }

        if (_collect == true) {
            Display::SurfaceImplementation* entry = new Display::SurfaceImplementation(*this, id, name);

            // manual increase the refcount for the _waylandSurfaces map.
            entry->AddRef();

            // Somewhere, someone, created a surface, register it.
            _surfaces.insert(std::pair<uint32_t, Display::SurfaceImplementation*>(id, entry));
        }

        if (_clientHandler != nullptr) {
            _clientHandler->Attached(id);
        }
        _adminLock.Unlock();
    }

    void Display::Dimensions(
        const uint32_t id,
        const uint32_t visible,
        const int32_t x, const int32_t y, const int32_t width, const int32_t height,
        const uint32_t opacity,
        const uint32_t zorder)
    {
        Trace("Updated Dimensions surfaceId=%d width=%d  height=%d x=%d, y=%d visible=%d opacity=%d zorder=%d\n", id, width, height, x, y, visible, opacity, zorder);
        _adminLock.Lock();

        SurfaceMap::iterator index = _surfaces.find(id);

        if (index != _surfaces.end()) {
            Trace("Updated Dimensions surfaceId=%d name=%s width=%d  height=%d x=%d, y=%d visible=%d opacity=%d zorder=%d\n", id, index->second->Name().c_str(), width, height, x, y, visible, opacity, zorder);
            index->second->Dimensions(visible, x, y, width, height, opacity, zorder);
        } else {
            // TODO: Seems this is a surface, we did not create. maybe we need to collect it in future.
            //Trace("Unidentified surface: id=%d.\n");
        }

        _adminLock.Unlock();
    }

    void Display::Destructed(const uint32_t id)
    {
        _adminLock.Lock();

        if (_collect != true) {
            SurfaceMap::iterator index = _surfaces.find(id);

            if (index != _surfaces.end()) {
                // See if it is in the surfaces map, we need to take it out here as well..
                WaylandSurfaceMap::iterator entry(_waylandSurfaces.find(index->second->_surface));

                // assert(entry != _waylandSurfaces.end());

                if (entry != _waylandSurfaces.end()) {
                    entry->second->Release();
                    _waylandSurfaces.erase(entry);
                }

                if (_keyboardReceiver == index->second) {
                    _keyboardReceiver = nullptr;
                }

                index->second->Unlink();
                index->second->Release();
                _surfaces.erase(index);
            }

        }
        if (_clientHandler != nullptr) {
            _clientHandler->Detached(id);
        }
        _adminLock.Unlock();
    }

    /* static */ Display& Display::Instance(const std::string& displayName)
    {
        if (_runtimeDir.empty() == true) {
            const char* envName = ::getenv("XDG_RUNTIME_DIR");
            if (envName != nullptr) {
                _runtimeDir = envName;
            }
        }

        // Please define a runtime directory, or using an environment variable (XDG_RUNTIME_DIR)
        // or by setting it before creating a Display, using Display::RuntimeDirectory(<DIR>),
        // or by passing it as an argument to this method (none empty dir)
        assert(_runtimeDir.empty() == false);

        Display* result(nullptr);

        _adminLock.Lock();

        DisplayMap::iterator index(_displays.find(displayName));

        if (index == _displays.end()) {
            result = new Display(displayName);
            _displays.insert(std::pair<const std::string, Display*>(displayName, result));
        } else {
            result = index->second;
        }
        result->AddRef();
        _adminLock.Unlock();

        assert(result != nullptr);

        return (*result);
    }

    static void signalHandler(int signum)
    {
    }

    void Display::Process(Display::IProcess* processloop)
    {

        struct sigaction sigint;

        sigint.sa_handler = signalHandler;
        sigemptyset(&sigint.sa_mask);
        sigint.sa_flags = SA_RESETHAND;
        sigaction(SIGINT, &sigint, nullptr);

        _thread = ::pthread_self();

        Trace("Setup dispatch loop using thread %p signal: %d \n", &_thread, _signal);
        if (_display != nullptr) {
            while ((wl_display_dispatch(_display) != -1) && (processloop->Dispatch() == true)) {
                /* intentionally left empty */
            }
        }
    }

    int Display::Process(const uint32_t data)
    {

        signed int result(0);
        _adminLock.Lock();
        if (_display) {

            while (wl_display_prepare_read(_display) != 0) {
                if (wl_display_dispatch_pending(_display) < 0) {
                    result = -1;
                    break;
                }
            }

            wl_display_flush(_display);

            if (data != 0) {
                if (wl_display_read_events(_display) < 0) {
                    result = -2;
                } else {
                    if (wl_display_dispatch_pending(_display) < 0) {
                        result = 1;
                    }
                }
            } else {
                wl_display_cancel_read(_display);
                result = -3;
            }
        }
        _adminLock.Unlock();
        return result;
    }

    void Display::AddRef() const
    {
        if (Core::InterlockedIncrement(_refCount) == 1) {
            const_cast<Display*>(this)->Initialize();
        }
        return;
    }

    uint32_t Display::Release() const
    {
        if (Core::InterlockedDecrement(_refCount) == 0) {
            const_cast<Display*>(this)->Deinitialize();

            //Indicate Wayland connection is closed properly
            return (Core::ERROR_CONNECTION_CLOSED);
        }
        return (Core::ERROR_NONE);
    }

    int Display::FileDescriptor() const
    {
        return (wl_display_get_fd(_display));
    }

    void Display::Signal()
    {
        printf("Received Signal, killing thread %p.\n", &_thread);
        ::pthread_kill(_thread, SIGINT);
    }
}

/* static */ Compositor::IDisplay* Compositor::IDisplay::Instance(const std::string& displayName)
{
    return (&(Wayland::Display::Instance(displayName)));
}
}
