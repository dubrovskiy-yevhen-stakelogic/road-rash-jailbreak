#pragma once
// The shader sources of the renderer, shared by `rrview` and `rrgame`.
//
// They are shared rather than copied on purpose: the per-pixel checks `rrview` runs
// (`--texcheck`, `--cellcheck`, `--bikecheck`, `--skycheck`, `--skygradcheck`) prove that THESE
// shaders sample what the binding rules predict. A second copy in the game would be unproven.
#include "render/gl_api.h"

#include <string>

namespace rr::render {

// The main indexed-texture program: models, roadside props, scene cells, the road ribbon.
extern const char* const kVertexShader;
extern const char* const kFragmentShader;

// The type-4 panorama: a direct-colour image on a cylinder.
extern const char* const kSkyVertexShader;
extern const char* const kSkyFragmentShader;

// The Gouraud sky above the skyline: a screen-space strip, no view matrix.
extern const char* const kGradVertexShader;
extern const char* const kGradFragmentShader;

// The cloud ring (rrformats/sky_clouds.h): an RGBA image whose alpha carries the PS1 STP rule,
// interpolated affinely (noperspective) the way the GPU interpolates a POLY_FT4.
extern const char* const kCloudVertexShader;
extern const char* const kCloudFragmentShader;

// The bike's shadow: flat quads projected onto the ground, drawn with the GPU's subtractive mode.
extern const char* const kShadowVertexShader;
extern const char* const kShadowFragmentShader;

// The PS1 look: the finished frame quantised to 15-bit colour with the GPU's 4x4 dither matrix.
extern const char* const kPostVertexShader;
extern const char* const kPostFragmentShader;

// The HUD: screen-space RGBA quads out of the decoded `DASH?P.TEX` page.
extern const char* const kHudVertexShader;
extern const char* const kHudFragmentShader;

// On OpenGL ES (gl_api.h RR_GLES) CompileShader passes every source through EsShaderSource first: the desktop
// `#version 330 core` line becomes `#version 320 es` with highp defaults and the extensions `caps` offers. A pure
// function, available on every platform so a desktop tool can hand its output to a GLSL ES validator.
std::string EsShaderSource(GLenum type, const char* source, const GlCaps& caps);
GLuint CompileShader(GLenum type, const char* source);
GLuint BuildProgram(const char* vertexSource, const char* fragmentSource);

} // namespace rr::render
