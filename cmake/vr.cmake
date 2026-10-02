# VR: the OpenXR layer on OpenGL / OpenGL ES and the proof target rrvrtest.
# Included at the end of the root CMakeLists.txt, after every library it adjusts is defined.
#
#   Windows:  rrxr (XR_KHR_opengl_enable) + rrvrtest.exe; openxr_loader.dll is loaded at run time from next to the
#             executable - an existing copy on this machine is copied there (RR_OPENXR_LOADER_DLL), never committed.
#   Android:  the Quest build (scripts/build-quest.ps1 configures this tree with the NDK toolchain): the portable
#             libraries with OpenGL ES 3.2 (render/gl_api.h RR_GLES), the AAudio device, rrxr (XR_KHR_opengl_es_enable)
#             and librrvrtest.so, a NativeActivity. The OpenXR loader is libopenxr_loader.so of the Khronos
#             openxr_loader_for_android AAR already on disk (RR_OPENXR_ANDROID_AAR), extracted into the build tree.

set(RR_OPENXR_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/include")
target_sources(rrplatform PRIVATE src/platform/app_paths.cpp)

if(ANDROID)
    enable_language(C) # android_native_app_glue.c
    # Warnings stay errors (the root's -Wall -Wextra -Werror). Clang has a few diagnostic classes MSVC's /W4 does not,
    # which the Windows build - the project's warnings gate - therefore never reports: unused internal constants /
    # functions / private fields / lambda captures and '&' inside '|' without parentheses. They are switched off for
    # the Quest build only, so a desktop-clean change never breaks the APK over them.
    set(RR_CLANG_ONLY_WARNINGS -Wno-unused-const-variable -Wno-unused-function -Wno-unused-private-field
        -Wno-unused-lambda-capture -Wno-bitwise-op-parentheses)
    foreach(t rrvfs rrformats rrgamesim rrgameaudio rrgamecore rrgameshell rrrender rrplatform)
        target_compile_options(${t} PRIVATE ${RR_CLANG_ONLY_WARNINGS})
    endforeach()
    # The platform layer's portable part: the desktop's WinMM device and the XInput / HID controller code stay on
    # Windows (the Quest's controllers come through OpenXR actions, platform/xr/xr_pad.h, mapped by the bindings of
    # the portable input_bindings.cpp).
    set_property(TARGET rrplatform PROPERTY SOURCES
        src/platform/png.cpp src/platform/app_paths.cpp src/platform/audio_device_aaudio.cpp
        src/platform/input_bindings.cpp)
    set_property(TARGET rrplatform PROPERTY LINK_LIBRARIES aaudio log)
    set_property(TARGET rrplatform PROPERTY INTERFACE_LINK_LIBRARIES aaudio log)
    # The renderer without the Win32 window and against OpenGL ES 3.2 instead of opengl32.
    # window_win32.cpp is the desktop window; text_overlay.cpp writes with its built-in font there (no GDI: the VR
    # menu's text); render_target.cpp is the desktop backend of render_target.h (WGL, the window, an opengl32 import
    # patch) and render_target_es.cpp its ES one.
    get_target_property(RR_RENDER_SOURCES rrrender SOURCES)
    list(REMOVE_ITEM RR_RENDER_SOURCES src/render/window_win32.cpp src/render/render_target.cpp)
    list(APPEND RR_RENDER_SOURCES src/render/render_target_es.cpp)
    set_property(TARGET rrrender PROPERTY SOURCES ${RR_RENDER_SOURCES})
    get_target_property(RR_RENDER_LIBS rrrender LINK_LIBRARIES)
    list(REMOVE_ITEM RR_RENDER_LIBS opengl32)
    list(APPEND RR_RENDER_LIBS GLESv3 EGL)
    set_property(TARGET rrrender PROPERTY LINK_LIBRARIES ${RR_RENDER_LIBS})
    set_property(TARGET rrrender PROPERTY INTERFACE_LINK_LIBRARIES ${RR_RENDER_LIBS})

    # the loader of the Khronos AAR (headers: third_party/openxr, the same API); default: next to the source folder
    set(RR_OPENXR_ANDROID_AAR "${CMAKE_SOURCE_DIR}/../openxr_loader_for_android-1.1.43.aar" CACHE FILEPATH
        "openxr_loader_for_android AAR whose arm64-v8a libopenxr_loader.so the APK packages")
    if(NOT EXISTS "${RR_OPENXR_ANDROID_AAR}")
        message(FATAL_ERROR "RR_OPENXR_ANDROID_AAR: ${RR_OPENXR_ANDROID_AAR} not found")
    endif()
    set(RR_OPENXR_AAR_DIR "${CMAKE_BINARY_DIR}/openxr_aar")
    if(NOT EXISTS "${RR_OPENXR_AAR_DIR}/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so")
        file(ARCHIVE_EXTRACT INPUT "${RR_OPENXR_ANDROID_AAR}" DESTINATION "${RR_OPENXR_AAR_DIR}")
    endif()
    add_library(rr_openxr_loader SHARED IMPORTED)
    set_target_properties(rr_openxr_loader PROPERTIES
        IMPORTED_LOCATION "${RR_OPENXR_AAR_DIR}/prefab/modules/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so"
        IMPORTED_NO_SONAME OFF)

    add_library(rrxr STATIC src/platform/xr/xr_session_gl.cpp src/platform/xr/xr_actions.cpp
                            src/platform/xr/egl_context_android.cpp)
    target_include_directories(rrxr PUBLIC src "${RR_OPENXR_INCLUDE}")
    target_link_libraries(rrxr PUBLIC rrrender rr_openxr_loader EGL GLESv3 log)
    # XrFoo x{XR_TYPE_FOO} - the OpenXR idiom - leaves `next` and the rest value-initialised on purpose
    target_compile_options(rrxr PRIVATE -Wno-missing-field-initializers)

    add_library(rr_native_glue STATIC "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
    target_compile_options(rr_native_glue PRIVATE -Wno-unused-parameter)
    target_include_directories(rr_native_glue PUBLIC "${ANDROID_NDK}/sources/android/native_app_glue")

    add_library(rrvrtest SHARED tools/rrvrtest/main_android.cpp tools/rrvrtest/vr_loop.cpp tools/rrvrtest/vr_scene.cpp)
    target_include_directories(rrvrtest PRIVATE tools/rrvrtest)
    target_link_libraries(rrvrtest PRIVATE rrxr rrrender rrgamecore rrgamesim rrgameaudio rrformats rrvfs rrplatform
                                           rr_native_glue android log EGL GLESv3)
    target_link_options(rrvrtest PRIVATE "-Wl,-u,ANativeActivity_onCreate")

    # THE QUEST GAME: librrgame.so - rrgame's own portable loops (the front
    # end, the career, the races, the saves) over the VR host, entered from the NativeActivity (main_android.cpp).
    # The desktop entry (main_desktop.cpp, game_host_desktop.cpp) and the desktop controllers' check (padmap_check.cpp)
    # stay on Windows.
    get_target_property(RR_GAME_SOURCES rrgame SOURCES)
    list(REMOVE_ITEM RR_GAME_SOURCES tools/rrgame/main_desktop.cpp tools/rrgame/game_host_desktop.cpp
                                     tools/rrgame/padmap_check.cpp)
    add_library(rrgame_quest SHARED ${RR_GAME_SOURCES} tools/rrgame/main_android.cpp)
    set_target_properties(rrgame_quest PROPERTIES OUTPUT_NAME rrgame)
    target_include_directories(rrgame_quest PRIVATE tools/rrgame)
    target_link_libraries(rrgame_quest PRIVATE rrxr rrgameshell rrrender rrgamecore rrgamesim rrgameaudio rrformats
                                               rrvfs rrplatform rr_native_glue android log EGL GLESv3)
    target_compile_options(rrgame_quest PRIVATE ${RR_CLANG_ONLY_WARNINGS})
    target_link_options(rrgame_quest PRIVATE "-Wl,-u,ANativeActivity_onCreate")

    # The APK's native libraries, gathered where android/app/build.gradle packages them from: the game and the
    # Khronos loader (scripts/build-quest.ps1 empties the folder first, so only what is built now is packaged).
    set(RR_JNILIBS_DIR "${CMAKE_BINARY_DIR}/jniLibs/${ANDROID_ABI}")
    add_custom_command(TARGET rrgame_quest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RR_JNILIBS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:rrgame_quest>" "${RR_JNILIBS_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RR_OPENXR_AAR_DIR}/prefab/modules/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so"
                "${RR_JNILIBS_DIR}/"
        VERBATIM)
elseif(WIN32)
    add_library(rrxr STATIC src/platform/xr/xr_session_gl.cpp src/platform/xr/xr_actions.cpp)
    target_include_directories(rrxr PUBLIC src PRIVATE "${RR_OPENXR_INCLUDE}")
    target_link_libraries(rrxr PUBLIC rrrender opengl32)
    # rrgame --vr / --vr-mock (tools/rrgame/game_host_vr*.cpp): the OpenXR session is linked in; the loader DLL is
    # opened at run time only when --vr asks for it, so the desktop game never needs it.
    target_link_libraries(rrgame PRIVATE rrxr)

    add_executable(rrvrtest tools/rrvrtest/main_win32.cpp tools/rrvrtest/vr_loop.cpp tools/rrvrtest/vr_scene.cpp)
    target_include_directories(rrvrtest PRIVATE tools/rrvrtest)
    target_link_libraries(rrvrtest PRIVATE rrxr rrrender rrgamecore rrgamesim rrgameaudio rrformats rrvfs rrplatform opengl32)

    # An openxr_loader.dll that is already on this machine (the Khronos loader the gt2-play project ships with its
    # builds); rrvrtest also finds one on PATH. Nothing is downloaded.
    set(RR_OPENXR_LOADER_DLL "" CACHE FILEPATH "openxr_loader.dll copied next to rrvrtest.exe")
    if(NOT RR_OPENXR_LOADER_DLL)
        foreach(candidate "${CMAKE_SOURCE_DIR}/../gt2-play/runtime/openxr_loader.dll" "${CMAKE_SOURCE_DIR}/../gt2-play/build/tools/xrsim/openxr_loader.dll")
            if(EXISTS "${candidate}" AND NOT RR_OPENXR_LOADER_DLL)
                set(RR_OPENXR_LOADER_DLL "${candidate}")
            endif()
        endforeach()
    endif()
    if(RR_OPENXR_LOADER_DLL)
        add_custom_command(TARGET rrvrtest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RR_OPENXR_LOADER_DLL}" "$<TARGET_FILE_DIR:rrvrtest>/openxr_loader.dll"
            VERBATIM)
        add_custom_command(TARGET rrgame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RR_OPENXR_LOADER_DLL}" "$<TARGET_FILE_DIR:rrgame>/openxr_loader.dll"
            VERBATIM)
    else()
        message(STATUS "rrvrtest: no openxr_loader.dll found; put one next to rrvrtest.exe to run it")
    endif()
endif()
