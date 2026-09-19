// libgodot_host: standalone executable whose main() owns thread 0 so
// libgodot's macOS init (which touches NSApplication) is on the main
// thread as AppKit requires.
//
// All communication is over weft::harness (iceoryx2 shared memory).
// Lifecycle commands from elixir and P2P frames between godot hosts use
// the same framing — DYNAMIC-payload pub/sub, byte-slice in, byte-slice
// out — differing only in service name. The BEAM never sees the hot
// path; it drives lifecycle through a weft-harness client.
//
// Modes:
//   --smoke     one create/start/iterate/stop cycle, exit 0
//   (default)   weft::run_command_loop dispatching lifecycle opcodes

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/classes/godot_instance.hpp>

#if defined(LIBGODOT_HOST_HAS_WEFT_HARNESS)
#include "weft/command.hpp"
#include "weft/loop.hpp"
#include "p2p_bus.hpp"
#endif

// libgodot exports (see 4-entities/entities-godot/core/extension/libgodot.h).
// Only create/destroy are C-exports; start/iteration/stop/focus_*/pause/
// resume live on GodotInstance and are reached through godot-cpp bindings.
typedef GDExtensionObjectPtr (*libgodot_create_fn)(int, char *[], GDExtensionInitializationFunction);
typedef void (*libgodot_destroy_fn)(GDExtensionObjectPtr);

// GDExtension entry-point: register nothing (the host does not own scene
// classes), but hand godot-cpp a valid InitObject so its runtime registers
// its own bookkeeping — otherwise reinterpret_cast'ing the returned
// GodotInstance pointer through godot-cpp will read from an uninitialized
// binding.
static void host_initialize_module(godot::ModuleInitializationLevel p_level) {
#if defined(LIBGODOT_HOST_HAS_WEFT_HARNESS)
    // Register the P2P bus class at SCENE level so GDScript can `P2PBus.new()`.
    // Only useful when the underlying services actually opened — GDScript can
    // check via bus.is_ready(). Guarded by g_p2p.ready so the class does not
    // exist in --smoke mode (which never calls open_p2p) — a bound class that
    // dereferences the un-opened publisher/subscriber would crash on send/recv.
    if (p_level == godot::MODULE_INITIALIZATION_LEVEL_SCENE && libgodot_host::g_p2p.ready) {
        libgodot_host::register_p2p_bus();
    }
#else
    (void)p_level;
#endif
}
static void host_uninitialize_module(godot::ModuleInitializationLevel /*p_level*/) {}

#if defined(LIBGODOT_HOST_HAS_WEFT_HARNESS)
// Definition of the P2P singleton declared extern in p2p_bus.hpp.
namespace libgodot_host { P2PState g_p2p; }
#endif

extern "C" GDExtensionBool GDE_EXPORT host_gdextension_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization *r_initialization) {
    godot::GDExtensionBinding::InitObject init_object(p_get_proc_address, p_library, r_initialization);
    init_object.register_initializer(host_initialize_module);
    init_object.register_terminator(host_uninitialize_module);
    init_object.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_object.init();
}

static std::string resolve_libgodot_path(const char *arg0) {
    if (const char *env = getenv("LIBGODOT_PATH")) return env;
    std::string self = arg0;
    auto slash = self.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "." : self.substr(0, slash);
    return dir + "/../../build/libgodot.dylib";
}

static void die(const char *what) {
    const char *err = dlerror();
    fprintf(stderr, "libgodot_host: %s: %s\n", what, err ? err : "unknown");
    exit(2);
}

// -------------------------------------------------------------------------
// Shared state driven by both the CLI smoke path and the bus loop path.
// A single global lets the free-function weft::Ask handler reach the same
// engine the process has already booted, without wrapping ctx around a
// tangle of small structs. The host is a one-instance driver by design
// (libgodot itself refuses a second create).
// -------------------------------------------------------------------------
struct HostState {
    void *lib_handle = nullptr;
    libgodot_create_fn create = nullptr;
    libgodot_destroy_fn destroy = nullptr;
    GDExtensionObjectPtr obj = nullptr;
    godot::GodotInstance *instance = nullptr;
    // argv storage kept alive for the whole engine lifetime — libgodot
    // captures pointers into it via Main::setup.
    std::vector<std::string> argv_storage;
    std::vector<char *> argv;
    uint64_t tick_count = 0;
};

static HostState g_host;

static void host_load_lib(const std::string &libpath) {
    fprintf(stderr, "libgodot_host: loading %s\n", libpath.c_str());
    g_host.lib_handle = dlopen(libpath.c_str(), RTLD_LAZY);
    if (!g_host.lib_handle) die("dlopen");
    g_host.create = (libgodot_create_fn)dlsym(g_host.lib_handle, "libgodot_create_godot_instance");
    if (!g_host.create) die("dlsym create");
    g_host.destroy = (libgodot_destroy_fn)dlsym(g_host.lib_handle, "libgodot_destroy_godot_instance");
    if (!g_host.destroy) die("dlsym destroy");
}

// Build godot argv from fields the outer caller controls (arg0,
// optional -s script, optional --path project, headless/headed toggle);
// ownership stays in g_host.argv_storage so godot's Main::setup can hold
// the pointers.
//
// Headless is the default because it is the only mode that works without
// a compositor: no DisplayServer, no audio driver open, no renderer
// context. Passing headless=false drops --headless so godot brings up
// its normal DisplayServer (DisplayServerEmbedded via libgodot on macOS,
// DisplayServer{Windows,Wayland,X11} on the other platforms) — the
// caller-visible flag for that is `--no-headless`, matching Godot's
// own `--headless` bool inverted with the `--no-*` convention. Callers
// pass headless=false only when they actually have a window server to
// render into; the host does no probing because getting that wrong
// (windowed on a headless CI runner, say) produces a hard crash inside
// Godot's platform init rather than a clean error.
static void host_build_argv(const char *arg0, const std::string &script,
                            const std::string &project, bool headless) {
    g_host.argv_storage.clear();
    g_host.argv.clear();
    g_host.argv_storage.emplace_back(arg0);
    if (headless) {
        g_host.argv_storage.emplace_back("--headless");
    }
    if (!script.empty()) {
        g_host.argv_storage.emplace_back("-s");
        g_host.argv_storage.emplace_back(script);
    }
    if (!project.empty()) {
        g_host.argv_storage.emplace_back("--path");
        g_host.argv_storage.emplace_back(project);
    }
    for (auto &s : g_host.argv_storage) g_host.argv.push_back(s.data());
}

// Boot godot on the current (main) thread. Returns true on success.
static bool host_create_instance() {
    if (g_host.obj) return true; // idempotent
    fprintf(stderr, "libgodot_host: libgodot_create_godot_instance on main thread\n");
    g_host.obj = g_host.create((int)g_host.argv.size(), g_host.argv.data(), host_gdextension_init);
    if (!g_host.obj) {
        fprintf(stderr, "libgodot_host: create returned nullptr (Main::setup failed?)\n");
        return false;
    }
    g_host.instance = reinterpret_cast<godot::GodotInstance *>(
            godot::internal::get_object_instance_binding(g_host.obj));
    fprintf(stderr, "libgodot_host: GodotInstance ptr %p\n", (void *)g_host.instance);
    return true;
}

static void host_destroy_instance() {
    if (!g_host.obj) return;
    // libgodot_destroy_godot_instance (platform/macos/libgodot_macos.mm:67)
    // already calls GodotInstance::stop() before teardown; calling stop()
    // here first double-stops and crashes destroy() at offset +32 with
    // KERN_INVALID_ADDRESS. Let destroy() own the whole wind-down.
    g_host.destroy(g_host.obj);
    g_host.obj = nullptr;
    g_host.instance = nullptr;
    g_host.tick_count = 0;
}

static volatile std::sig_atomic_t g_signalled = 0;

static void host_on_signal(int) { g_signalled = 1; }

int main(int argc, char *argv[]) {
    bool smoke = false;
    // --bus-dry-run opens the lifecycle + P2P services, prints their
    // readiness diagnostics, then returns 0 without entering
    // run_command_loop. Deterministic termination — no timeouts, no
    // signals — so smoke tests can assert the bus stood up without
    // needing a QUIT round-trip against a running loop.
    bool bus_dry_run = false;
    // Headless-by-default: the host's typical role is to serve elixir over
    // the bus with no window. --no-headless suppresses the flag so godot's
    // Main::setup brings up its normal DisplayServer — the same shape as
    // `--headless` in Godot's own CLI, negated by the `--no-*` convention
    // rather than invented terminology (Godot itself has no positive-form
    // flag; windowed is the default when --headless is absent).
    bool headless = true;
    int max_iterations = 8;
    std::string libpath;
    std::string script_path;
    std::string project_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--smoke") smoke = true;
        else if (a == "--bus-dry-run") bus_dry_run = true;
        else if (a == "--no-headless") headless = false;
        else if (a == "--headless") headless = true;
        else if (a == "--libgodot" && i + 1 < argc) libpath = argv[++i];
        else if (a == "--script" && i + 1 < argc) script_path = argv[++i];
        else if (a == "--project" && i + 1 < argc) project_path = argv[++i];
        else if (a == "--max-iterations" && i + 1 < argc) max_iterations = atoi(argv[++i]);
    }
    if (libpath.empty()) libpath = resolve_libgodot_path(argv[0]);

    host_load_lib(libpath);

    if (smoke) {
        // One-shot: build argv from CLI, create/start/iterate/stop/destroy, exit.
        host_build_argv(argv[0], script_path, project_path, headless);
        if (!host_create_instance()) return 3;
        if (!g_host.instance->start()) {
            fprintf(stderr, "libgodot_host: start() returned false\n");
            host_destroy_instance();
            return 4;
        }
        fprintf(stderr, "libgodot_host: started\n");
        int ticks = 0;
        while (ticks < max_iterations) {
            bool quit = g_host.instance->iteration();
            ticks++;
            if (quit) {
                fprintf(stderr, "libgodot_host: iteration %d requested quit\n", ticks);
                break;
            }
        }
        fprintf(stderr, "libgodot_host: ran %d iteration(s)\n", ticks);
        host_destroy_instance();
        fprintf(stderr, "libgodot_host: smoke OK\n");
        return 0;
    }

#if defined(LIBGODOT_HOST_HAS_WEFT_HARNESS)
    // P2P bus: open before entering the lifecycle loop so GDScript in the
    // running project can send/recv frames from tick 0. Failure to open
    // is logged but not fatal — a host without peers still serves elixir
    // via the lifecycle service, and P2PBus.send/recv become no-ops.
    if (libgodot_host::open_p2p()) {
        fprintf(stderr, "libgodot_host: P2P bus ready ('%s' out, '%s' in)\n",
                libgodot_host::P2P_OUT_SERVICE_NAME, libgodot_host::P2P_IN_SERVICE_NAME);
    } else {
        fprintf(stderr, "libgodot_host: P2P bus not ready; send/recv will be no-ops\n");
    }

    if (bus_dry_run) {
        // Every side has been opened; a live client-driven test can now
        // proceed against the running services. --bus-dry-run stops here
        // so the smoke suite can assert the readiness diagnostics without
        // needing a QUIT round-trip against an active run_command_loop.
        fprintf(stderr, "libgodot_host: bus dry-run OK\n");
        return 0;
    }

    // Bus mode: open the lifecycle service and dispatch opcodes until QUIT.
    fprintf(stderr, "libgodot_host: entering weft::run_command_loop on service '%s'\n",
            weft::COMMAND_SERVICE_NAME);
    // The bus loop drives godot::iteration() only when elixir asks (ITERATE
    // opcode). This puts the tick rate in the manager's hands and keeps the
    // process idle when no one is asking, which is what a lifecycle-managed
    // host should do.
    //
    // The Ask handler stashes prep args from --script/--project as the
    // default for CREATE when the payload is empty; a real client sends
    // its own argv over the wire.
    struct AskCtx {
        std::string default_script;
        std::string default_project;
        const char *arg0;
        bool default_headless;
    };
    AskCtx ctx{script_path, project_path, argv[0], headless};

    auto ask = [](void *ctx_v, const char *command, size_t len,
                  unsigned char *reply, size_t cap, int *stop) -> size_t {
        auto *ac = static_cast<AskCtx *>(ctx_v);
        if (len == 0 || cap < 1) return 0;
        const uint8_t opcode = static_cast<uint8_t>(command[0]);
        const char *body = command + 1;
        const size_t body_len = len - 1;

        auto write_ok = [&](size_t extra) -> size_t {
            reply[0] = 0x00; // ok
            return 1 + extra;
        };
        auto write_err = [&](const char *msg) -> size_t {
            reply[0] = 0x01; // err
            const size_t n = std::strlen(msg);
            const size_t room = cap - 1;
            const size_t copy = n < room ? n : room;
            std::memcpy(reply + 1, msg, copy);
            return 1 + copy;
        };

        switch (opcode) {
        case 0x01: { // CREATE — body is NUL-separated argv
            if (g_host.obj) return write_err("already_created");
            // Parse body into g_host argv_storage; fall back to CLI defaults if empty.
            g_host.argv_storage.clear();
            g_host.argv.clear();
            g_host.argv_storage.emplace_back(ac->arg0);
            if (body_len > 0) {
                const char *p = body;
                const char *end = body + body_len;
                while (p < end) {
                    const char *nul = static_cast<const char *>(std::memchr(p, '\0', end - p));
                    size_t seg = nul ? (size_t)(nul - p) : (size_t)(end - p);
                    g_host.argv_storage.emplace_back(p, seg);
                    p = nul ? nul + 1 : end;
                }
            } else {
                if (ac->default_headless) {
                    g_host.argv_storage.emplace_back("--headless");
                }
                if (!ac->default_script.empty()) {
                    g_host.argv_storage.emplace_back("-s");
                    g_host.argv_storage.emplace_back(ac->default_script);
                }
                if (!ac->default_project.empty()) {
                    g_host.argv_storage.emplace_back("--path");
                    g_host.argv_storage.emplace_back(ac->default_project);
                }
            }
            for (auto &s : g_host.argv_storage) g_host.argv.push_back(s.data());
            if (!host_create_instance()) return write_err("create_failed");
            return write_ok(0);
        }
        case 0x02: { // START
            if (!g_host.instance) return write_err("no_instance");
            if (!g_host.instance->start()) return write_err("start_failed");
            return write_ok(0);
        }
        case 0x03: { // ITERATE — reply body: [quit:u8][tick:u64 LE]
            if (!g_host.instance) return write_err("no_instance");
            if (cap < 1 + 1 + 8) return write_err("cap_too_small");
            // Drain any P2P frames the subscriber has buffered before
            // godot ticks, so GDScript sees this tick's inbound frames
            // via P2PBus.recv() during _process.
            (void)libgodot_host::drain_p2p_inbox();
            bool quit = g_host.instance->iteration();
            g_host.tick_count++;
            reply[0] = 0x00;
            reply[1] = quit ? 1 : 0;
            std::memcpy(reply + 2, &g_host.tick_count, 8);
            return 1 + 1 + 8;
        }
        case 0x04: { // STOP
            if (g_host.instance) g_host.instance->stop();
            return write_ok(0);
        }
        case 0x05: { // DESTROY (idempotent)
            host_destroy_instance();
            return write_ok(0);
        }
        case 0x7F: { // QUIT — tear down and end the loop
            host_destroy_instance();
            *stop = 1;
            return write_ok(0);
        }
        default:
            return write_err("unknown_opcode");
        }
    };

    std::signal(SIGTERM, host_on_signal);
    std::signal(SIGINT, host_on_signal);

    int rc = weft::run_command_loop(&ctx, ask, 10'000'000, &g_signalled);

    // The loop leaves on a signal without running the QUIT handler, and the
    // instance has to go before the process does.
    if (g_signalled) {
        fprintf(stderr, "libgodot_host: signalled, tearing down\n");
        host_destroy_instance();
    }
    fprintf(stderr, "libgodot_host: run_command_loop returned %d\n", rc);
    return rc;
#else
    fprintf(stderr, "libgodot_host: built without LIBGODOT_HOST_HAS_WEFT_HARNESS; "
                    "either build against thirdparty/weft-harness/ or pass --smoke\n");
    return 5;
#endif
}
