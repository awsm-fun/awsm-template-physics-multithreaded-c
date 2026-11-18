cmake_minimum_required(VERSION 3.26)
project(awsm_app LANGUAGES C)

# The app thread: your game (src/game/) and the runtime that runs it
# (src/runtime/), in C, built natively (tests, headless runs, sanitizers) and
# with Emscripten for the browser (presets `wasm` and `wasm-dev`). See
# CMakePresets.json for the configurations.

# --- Language standard -------------------------------------------------------
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
# GNU C23 (-std=gnu23): strict -std=c23 hides POSIX (usleep, strdup, M_PI, ...)
# on Emscripten/Linux but not on macOS, so code would build natively and break
# only under wasm; it also rejects EM_ASM. -Wpedantic still flags GNU syntax.
# (GNU mode predefines `unix`/`linux` macros, so avoid those as identifiers.)
set(CMAKE_C_EXTENSIONS ON)

# Emit compile_commands.json so clangd / editors understand the project.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Shared warnings / sanitizer settings ------------------------------------
add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wstrict-prototypes -Wmissing-prototypes
    -Werror=implicit-function-declaration
)

if(EMSCRIPTEN)
    # emcc adds its target, sysroot and extra system include dirs internally, so
    # they never reach compile_commands.json and clangd sees the wrong headers.
    # Ask emcc for them (`emcc --cflags`) and repeat the ones clangd needs. This
    # is a no-op for emcc itself, but makes the compile database self-describing:
    # any clangd, in any editor, gets it right with no per-machine configuration.
    # Recognised: target, sysroot and system include dirs, whatever their values
    # in future versions. Backend-only flags (-mllvm ...) are skipped.
    # Note these are real compile options: if you ever switch to wasm64
    # (-sMEMORY64), drop the wasm32 --target here or emcc will refuse it.
    execute_process(COMMAND "${CMAKE_C_COMPILER}" --cflags
        OUTPUT_VARIABLE _em_cflags RESULT_VARIABLE _em_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(_em_editor_flags "")
    if(_em_rc EQUAL 0)
        # Homebrew: emcc reports its sysroot under Cellar/emscripten/<version>/;
        # use the version-independent opt/ symlink so the compile database
        # (and the editor) keep working across `brew upgrade`.
        string(REGEX REPLACE "/Cellar/emscripten/[^/ ]+/" "/opt/emscripten/"
            _em_cflags_stable "${_em_cflags}")
        if(_em_cflags_stable MATCHES "--sysroot=([^ ]+)")
            # (Nested: ${CMAKE_MATCH_1} would expand before MATCHES runs.)
            if(IS_DIRECTORY "${CMAKE_MATCH_1}")
                set(_em_cflags "${_em_cflags_stable}")
            endif()
        endif()
        separate_arguments(_em_cflags UNIX_COMMAND "${_em_cflags}")
        set(_prev "")
        foreach(_f IN LISTS _em_cflags)
            if(_prev STREQUAL "-target")
                list(APPEND _em_editor_flags "--target=${_f}")
            elseif(_f MATCHES "^--target=")
                list(APPEND _em_editor_flags "${_f}")
            elseif(_f MATCHES "^--sysroot=")
                list(APPEND _em_editor_flags "${_f}")
            elseif(_prev STREQUAL "-Xclang" AND _f MATCHES "^-(iwithsysroot|isystem|idirafter)")
                # Use the single-token driver form: CMake de-duplicates
                # repeated options, which would mangle -Xclang pairs.
                list(APPEND _em_editor_flags "${_f}")
            endif()
            set(_prev "${_f}")
        endforeach()
    endif()
    # Fallbacks, each independent, in case emcc's output format ever changes.
    if(NOT _em_editor_flags MATCHES "--target=")
        list(APPEND _em_editor_flags --target=wasm32-unknown-emscripten)
    endif()
    if(NOT _em_editor_flags MATCHES "--sysroot=" AND DEFINED EMSCRIPTEN_SYSROOT)
        list(APPEND _em_editor_flags "--sysroot=${EMSCRIPTEN_SYSROOT}")
    endif()
    target_compile_options(project_options INTERFACE ${_em_editor_flags})

    # Re-run configure automatically (on the next build) when Emscripten is
    # upgraded, so the flags above and the compiler checks stay current.
    if(EXISTS "${EMSCRIPTEN_ROOT_PATH}/emscripten-version.txt")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${EMSCRIPTEN_ROOT_PATH}/emscripten-version.txt")
    endif()
endif()

# Link-time optimization for native Release builds, when the toolchain supports
# it (result cached; must come before any targets are defined). Not for wasm:
# emcc's -O3 link step already optimizes the whole program (binaryen), and on
# a program this size -flto makes the wasm slightly *larger*, not smaller.
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT EMSCRIPTEN)
    if(NOT DEFINED CACHE{APP_IPO_SUPPORTED})
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_ok LANGUAGES C)
        set(APP_IPO_SUPPORTED ${_ipo_ok} CACHE INTERNAL "LTO available")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${APP_IPO_SUPPORTED})
endif()

option(ENABLE_SANITIZERS "Build with AddressSanitizer + UBSan" OFF)
if(ENABLE_SANITIZERS)
    target_compile_options(project_options INTERFACE
        # no-sanitize-recover: UB aborts (so tests fail) instead of just printing.
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(project_options INTERFACE
        -fsanitize=address,undefined)
endif()

if(EMSCRIPTEN)
    # Shared memory and threads. The bridge block lives in this module's
    # memory, and the page and the renderer read it from other threads, so the
    # memory must be a SharedArrayBuffer. On top of that, Box3D's solver runs
    # on real threads (pthreads, which Emscripten implements as web workers
    # sharing this same memory).
    #  * -pthread does both: it makes the memory shared and links the
    #    threading runtime. wasm-ld refuses shared memory unless EVERY object
    #    was compiled for it, so the flag goes on all compiles, Box3D's
    #    included; that is why it is a directory-wide option, set before
    #    add_subdirectory() below.
    #  * -matomics -mbulk-memory are implied by -pthread, but spelling them
    #    out puts them in compile_commands.json, where clangd needs them (the
    #    futex wait in bridge.c is an atomics builtin).
    add_compile_options(-pthread -matomics -mbulk-memory)
    add_link_options(-pthread)
endif()

# --- Box3D ---------------------------------------------------------------------
# The real physics library (https://github.com/erincatto/box3d), unmodified,
# as a git submodule. Its own CMake project builds the `box3d` static library;
# because we are not its top-level project it skips its samples, tests and
# downloads.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendored/box3d/CMakeLists.txt")
    message(FATAL_ERROR
        "app/vendored/box3d is empty. Fetch the submodule with:\n"
        "  git submodule update --init --recursive\n"
        "(or run `task setup`).")
endif()
# SYSTEM: its headers are not held to this project's warning flags.
add_subdirectory(vendored/box3d EXCLUDE_FROM_ALL SYSTEM)
# Always optimized, even in Debug builds (native and wasm): an unoptimized
# solver struggles to keep up with 240 steps a second. It is compiled once and
# then left alone, so this costs nothing while iterating on the app. Asserts
# stay on in Debug (Box3D keys them off NDEBUG, not the optimization level).
# Note the sanitizers of the `debug` preset cover the app's code only: Box3D
# is built with its own flags, uninstrumented.
target_compile_options(box3d PRIVATE $<$<CONFIG:Debug>:-O2>)
if(NOT EMSCRIPTEN)
    # Box3D's scheduler uses pthreads; some systems need that linked explicitly.
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(box3d PUBLIC Threads::Threads)
endif()

# --- Library: the app itself, shared by the entry points and the tests --------
# Your game is every .c file in src/game/, picked up on the next build (no
# edit here when you add one). The runtime's files are listed. One library,
# because the two call each other (the runtime calls the game's hooks, the
# game calls runtime_*). Headers are included by their path under src/:
# "game/game.h", "runtime/runtime.h".
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS src/game/*.c)
add_library(core
    ${GAME_SOURCES}
    src/runtime/bridge.c
    src/runtime/camera.c
    src/runtime/fixed_step.c
    src/runtime/headless.c
    src/runtime/physics.c
    src/runtime/runtime.c
)
target_include_directories(core PUBLIC src)
target_link_libraries(core PUBLIC box3d PRIVATE project_options)
if(NOT EMSCRIPTEN)
    target_link_libraries(core PUBLIC m) # libm (part of libc under Emscripten)
endif()

# --- Entry point ----------------------------------------------------------------
# One target named `app` either way:
#   native  -> an executable that runs the app headless (src/runtime/main.c)
#   wasm    -> app.js + app.wasm for the browser worker (src/runtime/web_main.c)
if(EMSCRIPTEN)
    add_executable(app src/runtime/web_main.c)
    target_link_libraries(app PRIVATE core project_options)
    target_link_options(app PRIVATE
        # app.js is an ES module whose default export instantiates the wasm:
        #   import createApp from './app.js';  const mod = await createApp();
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        # Only ever loaded by app/web/worker.js, a module worker.
        -sENVIRONMENT=worker
        # Box3D's solver threads. Emscripten starts a thread by handing it to a
        # web worker, and creating a worker needs this thread's event loop,
        # which never runs again once app_run() starts. So the workers are
        # created up front, while the module loads: `await createApp()` only
        # resolves when all of them are ready. The count is a JavaScript
        # expression evaluated at load time: one per core beyond the three
        # threads that already exist (page, renderer, app), at most four, so a
        # small machine starts none. It must agree with solver_thread_count()
        # in runtime/web_main.c. STRICT=2 turns "needed a thread that was not
        # pre-created" into an error instead of a hang.
        "-sPTHREAD_POOL_SIZE=Math.max(0,Math.min(4,(navigator.hardwareConcurrency||1)-3))"
        -sPTHREAD_POOL_SIZE_STRICT=2
        # Box3D's step uses more stack than Emscripten's 64 KB default, on
        # this thread and on the solver threads.
        -sSTACK_SIZE=1MB
        -sDEFAULT_PTHREAD_STACK_SIZE=1MB
        # FIXED-size memory. The page and the renderer hold views of this
        # memory's buffer for the whole session; growing it would replace the
        # buffer under them. So it is sized once, here. 64 MB is far more than
        # this demo needs; raise it if your app allocates more (malloc
        # aborts with an out-of-memory error when it runs out). Must be a
        # multiple of 64 KB.
        -sINITIAL_MEMORY=64MB
        -sALLOW_MEMORY_GROWTH=0
        # The worker posts `mod.wasmMemory.buffer` (the SharedArrayBuffer) to
        # the page.
        -sEXPORTED_RUNTIME_METHODS=wasmMemory
        # No main(): JS calls the EMSCRIPTEN_KEEPALIVE exports in
        # runtime/web_main.c.
        --no-entry
        $<$<CONFIG:Debug>:-sASSERTIONS=1>
    )
    # The result is app.js + app.wasm in this preset's own build tree
    # (app/build/<preset>/). The page loads them from dist/app/, next to
    # web/worker.js; putting them there is a plain copy in taskfile/app.yml.
    # Linking straight into dist/ would have both wasm presets write the same
    # files, and Ninja cannot tell when another build tree has replaced them.
else()
    add_executable(app src/runtime/main.c)
    target_link_libraries(app PRIVATE core project_options)
endif()

# --- Tests --------------------------------------------------------------------
# (Under Emscripten, ctest runs these through node automatically.) Every
# tests/runtime/test_<name>.c and tests/game/test_<name>.c is a test called
# <name> (so a new test_*.c in tests/game/ needs no edit here). They include
# "runtime/check.h" from tests/.
enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/runtime/test_*.c tests/game/test_*.c)
foreach(_source IN LISTS TEST_SOURCES)
    get_filename_component(_target "${_source}" NAME_WE)
    string(REGEX REPLACE "^test_" "" _name "${_target}")
    add_executable(${_target} "${_source}")
    target_include_directories(${_target} PRIVATE tests)
    target_link_libraries(${_target} PRIVATE core project_options)
    add_test(NAME ${_name} COMMAND ${_target})
endforeach()

# The cross-language layout check: bridge_layout prints what THIS compiler
# makes of bridge.h, and the script compares the Rust and JS mirrors
# (renderer/src/bridge.rs, dom/runtime/bridge.js) with it.
add_executable(bridge_layout tests/runtime/bridge_layout.c)
target_link_libraries(bridge_layout PRIVATE core project_options)
find_program(NODE_EXECUTABLE node REQUIRED)
add_test(NAME bridge_mirrors
    COMMAND "${NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check-bridge.mjs"
            $<TARGET_FILE:bridge_layout>)
