#include "render/shaders.h"

#include "render/multiview.h"

#include <stdexcept>
#include <string>

namespace rr::render {

const char* const kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexel;      // absolute texel coordinates, as the .GEO stores them
layout(location = 3) in float aPalette;   // prim.tpage: which palette this primitive selects
layout(location = 4) in vec4 aShade;      // PS1 vertex colour (normalised u8) and, in .a, the mode
layout(location = 5) in vec4 aWindow;     // GP0(E2) texture window: origin x, y, tile size (0 = none)
layout(location = 6) in float aOt;        // the primitive's ordering-table key rule (rmd3.h TriangleSoup::Vertex::ot)
// The GTE projection (gte_proj.h): the vertex's point on the console's screen by the GTE's own RTPS - SX, SY, the view
// depth in cell units, and w = 2 when it is to be used (a vertex array without attribute 7 reads 0: the float camera).
layout(location = 7) in vec4 aScreen;
// Coplanar layers (coplanar.h): a decal primitive's lift - its plane's normal times how far the plate it
// covers lies from that plane (model units, turned by uModel); 0 for every other vertex (an array without attribute 8).
layout(location = 8) in vec3 aLift;
uniform int uGte;         // 1: aScreen places the vertices that carry one
uniform vec4 uGteMap;      // a console pixel -> NDC under the same projection as uViewProj
uniform vec2 uGteDepth;    // clip z of a view depth w: uGteDepth.x * w + uGteDepth.y
uniform float uGtePix;     // the GPU covers pixel (x, y) at its corner, GL at its centre
uniform int uEdgeMapOn;    // edges: the frame's projection is the GTE's (GteBegin) - a vertex at a console point keeps it
uniform samplerBuffer uOtScreen; // the static cell soup's aScreen, for its ordering-table keys
uniform int uOtScreenOn;
uniform mat4 uViewProj;
uniform mat4 uModel;
uniform vec3 uOrigin; // camera-relative transforms (multiview.h SetRenderOrigin): world - uOrigin
uniform int uOriginOn;
// The ordering-table order (race_scene.h SetOtOrder). The PS1 has no depth buffer: every
// primitive is linked into the frame's ordering table at a slot its depth key picks and the GPU paints the table from
// the far end, the first primitive linked into a slot last. With uOtOrder set, every vertex of a primitive gets the
// SAME depth - its slot, plus uOtRank / 1024 for the order it is linked in within one slot - so the depth test
// (GL_LESS, the first drawn winning a tie) resolves every overlap the way the table does, sort-order glitches included.
uniform int uOtOrder;
uniform samplerBuffer uOtVerts; // the vertex buffer being drawn, as floats (the other corners of this primitive)
uniform int uOtStride;          // floats per vertex
uniform float uOtRank;
uniform int uOtNear, uOtShift, uOtMax; // this pass's *(0x8005B4D4), *(0x8005B4D8), *(0x8005ADFC) - 2 (SLUS 0x80021988)
uniform float uOtBase; // 0.5 the first (far) pass of SLUS 0x80035958, 0 the second, painted over it
// Coplanar layers (race_scene_pc.cpp): x world units toward the eye along the vertex's own view ray (the
// picture keeps its place, only the depth moves; negative: away, the depth-fight detector's probe), y the clip z of the
// eye itself; x = 0 leaves the vertex alone. With x != 0 a vertex with a lift is pulled further by the lift's length over
// the cosine between its plane's normal and its own ray from uLayerEye - the plate's misfit measured along that ray.
uniform vec2 uLayerPull;
uniform vec3 uLayerEye;
float OtCamW(int id) {
    if (uOtScreenOn != 0) { // gteproj: the corner's MAC3 (cell units) where the GTE placed it
        vec4 s = texelFetch(uOtScreen, id);
        if (s.w > 1.5) return s.z / 64.0;
    }
    int b = id * uOtStride;
    vec4 p = vec4(texelFetch(uOtVerts, b).r, texelFetch(uOtVerts, b + 1).r, texelFetch(uOtVerts, b + 2).r, 1.0);
    return (uViewProj * (uModel * p)).w; // the view depth, world units
}
// The emitters' depth -> slot map (SLUS 0x800251E4 0x800256F8.., the scratchpad 0x1F800000..0x1F80001C of 0x800674D4):
// three linear pieces below 0x800 / 0x1000 / above with the shift 5 of every capture, clamped to [0, max].
int OtSlot(int t0) {
    int s;
    if (uOtNear < 4096) {
        int b = (t0 >> 11) > 0 ? 1 : 0;
        int c = (t0 >> 12) <= 0 ? (b == 0 ? -3 : -2) : (b == 0 ? -2 : 0);
        int a1 = uOtShift + c;
        int i = clamp(a1, 0, 3);
        int base = i == 2 ? 0x800 : (i == 3 ? 0x1000 : 0);
        int off = i == 2 ? 0x200 : (i == 3 ? 0x300 : 0);
        s = off + ((t0 - (base + uOtNear)) >> (a1 & 31));
    } else {
        s = (t0 - uOtNear) >> (uOtShift & 31);
    }
    return clamp(s, 0, uOtMax);
}
// The model light of the original (race_scene.h SetModelLight): 0 off (the old lambert), 1 a lit
// object - the vertex colour is ramp[clamp((L' . n) >> 19, 0, 31) >> shift] with n the model's own normal
// (4096 = 1.0) and L' the view's light (RASHCDG 0x80068468's two MVMVAs), 2 an unlit one - ramp[uUnlit >> shift].
// The texel is then MODULATED by it (colour / 128), as the GT3/GT4 packets of SLUS 0x800251E4 are.
uniform int uModelLight;
uniform int uLightX, uLightY, uLightZ;
uniform int uRampShift;
uniform int uUnlit;
uniform vec3 uRamp[32];
out vec3 vNormal;
out vec2 vTexelP;
out float vPalette;
out vec4 vShadeP;
// The same two, interpolated in SCREEN space: the PS1 GPU steps texels and Gouraud colours linearly across
// each triangle it draws (the near subdivision, scene_cell.md 13.8); the fragment stage takes these when uAffine.
noperspective out vec2 vTexelA;
noperspective out vec4 vShadeA;
// the same texel coordinates at the CENTROID - a point of the pixel the primitive covers. With MSAA a pixel
// the primitive only partly covers still runs its fragment at the pixel's centre, which may lie outside the primitive:
// the texel coordinate extrapolated past its corners reaches a neighbouring image of the atlas (the PS1 GPU samples only
// inside). A small distant car is mostly such edge pixels, and which neighbour texels they took changed with every
// sub-pixel move. The lookup takes these; the derivatives (the mip level) the centre ones (vTexelP / vTexelA), which
// stay smooth across the pixel quad.
centroid out vec2 vTexelPC;
noperspective centroid out vec2 vTexelAC;
flat out vec3 vWindow;
flat out float vOneSided; // aWindow.w: the primitive is one-sided (rmd3.h TriangleSoup::Vertex)
flat out float vObject;   // drawn with a model matrix of its own (a car, a rider, a prop - not the world's cells)
out vec3 eCon; // the console's fill rule (edge_rule.h): the console point (SX, SY) and z = 1 when the GTE placed the vertex
void main() {
    vec2 vTexel;
    vec4 vShade;
    vNormal = mat3(uModel) * aNormal;
    vTexel = aTexel;
    vPalette = aPalette;
    vShade = aShade;
    if (uModelLight != 0) {
        int step = uUnlit;
        if (uModelLight == 1) {
            ivec3 n = ivec3(round(aNormal * 4096.0));
            int mac = n.x * uLightX + n.y * uLightY + n.z * uLightZ; // MVMVA sf = 0: no shift
            step = mac < 0 ? 0 : min((mac / 256) / 2048, 31);          // >> 8, clamp >= 0, >> 11, clamp <= 31
        }
        step = step >> uRampShift;
        vShade = vec4(uRamp[step] / 255.0, 1.0 / 255.0);                // mode 1: modulate
        vNormal = vec3(0.0);                                             // no lambert on top
    }
    vWindow = aWindow.xyz;
    vOneSided = aWindow.w;
    vObject = uModel != mat4(1.0) ? 1.0 : 0.0;
    vTexelP = vTexel;
    vShadeP = vShade;
    vTexelA = vTexel;
    vTexelPC = vTexel;
    vTexelAC = vTexel;
    vShadeA = vShade;
    // camera-relative (multiview.h SetRenderOrigin): uViewProj is then the view-projection times a translation to uOrigin
    vec4 clip = uOriginOn != 0 ? uViewProj * vec4((uModel * vec4(aPos, 1.0)).xyz - uOrigin, 1.0) : uViewProj * uModel * vec4(aPos, 1.0);
    if (uGte != 0 && aScreen.w > 1.5) { // gteproj: the GTE's SXY, w the view depth in world units
        float w = aScreen.z / 64.0;
        vec2 p = aScreen.xy + vec2(uGtePix);
        clip = vec4((p.x * uGteMap.x + uGteMap.y) * w, (p.y * uGteMap.z + uGteMap.w) * w, uGteDepth.x * w + uGteDepth.y, w);
        if (aScreen.w > 2.5) clip = vec4(2.0, 2.0, 2.0, 1.0); // gteproj2: a triangle the GPU refuses (GteRejectBig)
    }
    // edges: a vertex the GTE placed, or one moved on the CPU to where the projection maps its SXY (the machines, the
    // objects - GteModelPoints), is a console point; anything else keeps GL's own coverage.
    eCon = vec3(aScreen.xy, 0.0);
    if (uGte != 0 && aScreen.w > 1.5 && aScreen.w < 2.5) {
        eCon.z = 1.0;
    } else if (uEdgeMapOn != 0) {
        if (clip.w > 0.0 && uGteMap.x != 0.0 && uGteMap.z != 0.0) {
            vec2 q = clip.xy / clip.w;
            vec2 c = vec2((q.x - uGteMap.y) / uGteMap.x, (q.y - uGteMap.w) / uGteMap.z) - vec2(uGtePix);
            vec2 r = floor(c + 0.5);
            if (abs(c.x - r.x) < 1.0 / 16.0 && abs(c.y - r.y) < 1.0 / 16.0) eCon = vec3(r, 1.0);
        }
    }
    if (uLayerPull.x != 0.0 && clip.w > 0.0) { // a decal pass (race_scene_pc.cpp BeginDecal / PcDrawLayers)
        float pull = uLayerPull.x;
        vec3 lift = mat3(uModel) * aLift;
        float h = length(lift);
        if (h > 0.0) {
            vec3 ray = (uModel * vec4(aPos, 1.0)).xyz - uLayerEye;
            float c = abs(dot(lift, ray)) / (h * max(length(ray), 1e-6));
            pull += h / max(c, 0.05); // at most twenty times the misfit (a plane seen edge-on)
        }
        // the depth of the point `pull` nearer along the ray (view depth w s): clip z = a w + b with b the eye's clip z,
        // so a w + b / s over the SAME w - x, y and w stay, and with them the perspective-correct interpolation of the
        // texels (moving w as well would shift the texture a little wherever the pull differs between the corners)
        float s = max(1.0 - pull / clip.w, 0.5);
        clip.z = clip.z - uLayerPull.y + uLayerPull.y / s;
    }
    gl_ClipDistance[0] = clip.z + clip.w; // the projection's near and far planes, kept when z is the slot below
    gl_ClipDistance[1] = clip.w - clip.z;
    if (uOtOrder != 0) {
        float key;
        if (aOt >= 0.0) {
            key = aOt;
        } else if (aOt > -1.5) { // a model primitive: six soup vertices from a multiple of 6, i0 i1 at +0 +1, i2 at +4
            int b = gl_VertexID - gl_VertexID % 6;
            key = (OtCamW(b) + OtCamW(b + 1) + 2.0 * OtCamW(b + 4)) * 16.0; // (z0 + z1 + 2 z2) >> 2, cell units
        } else { // a static cell primitive: the largest depth of its corners
            int code = int(-aOt + 0.5) - 2;
            int back = code % 8, size = code / 8;
            int b = gl_VertexID - back;
            float m = OtCamW(b);
            for (int k = 1; k < size; ++k) m = max(m, OtCamW(b + k));
            key = m * 64.0;
        }
        int slot = OtSlot(int(floor(clamp(key, -1.0e8, 1.0e8))));
        float depth = uOtBase + 0.5 * (float(slot) + uOtRank / 1024.0) / float(uOtMax + 2);
        clip.z = (depth * 2.0 - 1.0) * clip.w;
    }
    gl_Position = clip;
}
)";

const char* const kFragmentShader = R"(#version 330 core
in vec3 vNormal;
in vec2 vTexelP;
in float vPalette;
in vec4 vShadeP;
noperspective in vec2 vTexelA;
noperspective in vec4 vShadeA;
centroid in vec2 vTexelPC;              // at the centroid (the vertex stage says why)
noperspective centroid in vec2 vTexelAC;
uniform int uCentroidTexel; // 1 the lookups at the centroid (a multisampled picture), 0 at the centre
// 1: the texel and the colour as the PS1 GPU interpolates them, linearly in screen space (RRJB_AFFINE=off: 0).
uniform int uAffine;
flat in vec3 vWindow;
flat in float vOneSided;
flat in float vObject;
// gteproj (gte_proj.cpp GtePrograms): what the texel coordinate is rounded by before the lookup - the GPU steps u / v
// from the vertices' exact integers and TRUNCATES (1/256: GL's float interpolation that lands a hair under an exact
// integer); 0.5, to nearest, with RRJB_PROJ=float.
uniform float uTexRound;
uniform vec3 uTint;
uniform int uTextured;
// The model emitter's back-face test (SLUS 0x800251E4: GTE NCLIP over the corners i0, i1, i2, the
// primitive dropped when MAC0 < 0) for the primitives the soup marks one-sided. 0 = off (every
// draw that does not ask for it); +1 = the console's visible side is the one GL calls back-facing,
// -1 = the one GL calls front-facing. Which sign a draw needs depends only on the handedness of its
// model and view transforms, and race_scene.cpp states it where it sets it.
uniform int uNclip;
uniform sampler2D uIndex;     // one byte per pixel, the image as the disc stores it
uniform sampler2D uPalette;   // one row per palette
uniform vec2 uTexSize;
uniform float uPaletteCount;
uniform float uPaletteSize;
// 0 draws the frame; 1 reports (palette, index); 2 reports the draw id; 3 reports the shade the
// lit path applied; 4 means "this surface is not the subject of the check" - same geometry, same
// discard, but blue 0, so it can never be mistaken for a reported fragment.
uniform int uDebug;
uniform float uDebugId;   // which prop instance / cell range this draw is, for the readback check
// Smooth textures (smooth_texture.h): 1 samples the bound texture's RGBA
// expansion - one layer per palette row, mipmapped - bilinearly / trilinearly instead of the PS1's nearest index.
// uSmoothLayer maps the primitive's palette row to its layer + 1 (0: that row was not expanded -> nearest).
uniform int uSmooth;
uniform highp sampler2DArray uSmoothTex;
uniform sampler2D uSmoothLayer;
// how far an OBJECT's minified footprint is widened (race_scene_pc.cpp PcPrograms; 1 or less: as the
// derivatives give it, the control RRJB_SMOOTH_WIDEN=off). Trilinear filtering blends the level whose texels are just
// smaller than a pixel - a reconstruction kernel narrower than two pixels passes energy at the pixel frequency, and that
// aliases into the MEAN colour of a small object: a distant car's brightness went up and down with every sub-pixel move
// of the car or the eye (and differed between the two eyes). From 2 texels a pixel (by the minor axis, the one the
// anisotropic filter's level follows) the footprint grows smoothly with it up to uSmoothWiden times its size (sqrt(2):
// half a mip level, from 2.8 texels a pixel on). A car nearer than that (its texels 1..2 to a pixel) keeps its detail.
// Only objects (vObject: a model matrix of their own - cars, riders, pedestrians, props, weapons): a small object's
// mean is what blinks, while the world's cells are large and keep their sharper distant detail.
uniform float uSmoothWiden;
// The texel coordinate t (texel n spans [n, n + 1), as the GPU's floor(u)) filtered from layer `layer`; a texture
// window wraps every bilinear tap inside its tile while the texture is magnified or about 1:1, and is sampled
// clamped into the tile through the mipmaps when minified.
vec4 SmoothTexel(vec2 t, float layer, vec3 win, vec2 dx, vec2 dy, bool object) {
    float widen = object ? clamp(0.5 * min(length(dx), length(dy)), 1.0, max(uSmoothWiden, 1.0)) : 1.0;
    dx *= widen;
    dy *= widen;
    vec2 size = vec2(textureSize(uSmoothTex, 0).xy);
    if (win.z > 0.5) {
        float rho = max(length(dx), length(dy));
        if (rho <= 1.5) {
            vec2 p = t - 0.5;
            vec2 b = floor(p);
            vec2 f = p - b;
            ivec2 q00 = ivec2(mod(b, vec2(win.z)) + win.xy);
            ivec2 q11 = ivec2(mod(b + 1.0, vec2(win.z)) + win.xy);
            int l = int(layer);
            vec4 c00 = texelFetch(uSmoothTex, ivec3(q00.x, q00.y, l), 0);
            vec4 c10 = texelFetch(uSmoothTex, ivec3(q11.x, q00.y, l), 0);
            vec4 c01 = texelFetch(uSmoothTex, ivec3(q00.x, q11.y, l), 0);
            vec4 c11 = texelFetch(uSmoothTex, ivec3(q11.x, q11.y, l), 0);
            return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
        }
        vec2 w = clamp(mod(t, vec2(win.z)), vec2(0.5), vec2(win.z - 0.5)) + win.xy;
        return textureGrad(uSmoothTex, vec3(w / size, layer), dx / size, dy / size);
    }
    return textureGrad(uSmoothTex, vec3(t / size, layer), dx / size, dy / size);
}
out vec4 oColor;
void main() {
    vec2 vTexel = uAffine != 0 ? vTexelA : vTexelP;
    vec4 vShade = uAffine != 0 ? vShadeA : vShadeP;
    vec2 texelDx = dFdx(vTexel), texelDy = dFdy(vTexel); // in uniform control flow (the smooth textures' mipmaps)
    // the lookup at the centroid when the picture is multisampled (one sample: the centre, as it always was)
    vec2 texelAt = uCentroidTexel != 0 ? (uAffine != 0 ? vTexelAC : vTexelPC) : vTexel;
    if (uNclip != 0 && vOneSided > 0.5 && (gl_FrontFacing == (uNclip > 0))) discard;
    float index = 0.0;
    vec3 base = uTint;
    // The vertex colour the PlayStation GPU is given (docs\formats\scene_cell.md 13): mode 0 draws
    // the texel as it is, mode 1 modulates it by the colour (0x80 = 1.0, `texel * colour / 128`,
    // saturating), mode 2 is an untextured polygon of that flat colour. Quantised to the 1/255 grid
    // the framebuffer uses, so a check can predict the product exactly.
    // A vertex array that does not feed attribute 4 reads the GL default (0, 0, 0, 1), i.e. mode
    // 255, which must mean "as it is" - hence the exact comparisons.
    float shadeMode = floor(vShade.a * 255.0 + 0.5);
    bool modulate = abs(shadeMode - 1.0) < 0.5;
    bool flatColour = abs(shadeMode - 2.0) < 0.5;
    vec3 shade8 = floor(vShade.rgb * 255.0 + 0.5);
    if (flatColour) base = shade8 / 255.0;
    float smoothLayer = -1.0;
    if (uSmooth != 0 && uTextured != 0 && !flatColour && uDebug == 0)
        smoothLayer = floor(texelFetch(uSmoothLayer, ivec2(int(vPalette + 0.5), 0), 0).r * 255.0 + 0.5) - 1.0;
    if (smoothLayer >= 0.0) {
        vec4 texel = SmoothTexel(texelAt, smoothLayer, vWindow, texelDx, texelDy, vObject > 0.5);
        if (texel.a < 0.5) discard;
        base = texel.rgb;
        if (modulate) base = min(floor(base * 255.0 + 0.5) * shade8 / 128.0, vec3(255.0)) / 255.0;
    } else if (uTextured != 0 && !flatColour) {
        // The indexed image and its palettes are kept apart, the way the hardware keeps them: the
        // primitive's own tpage picks the row. Under a texture window only the low bits of the
        // texel coordinate survive and the tile origin is ORed in, so the UVs repeat in the tile.
        vec2 texel0 = floor(texelAt + uTexRound);
        if (vWindow.z > 0.5) texel0 = mod(texel0, vec2(vWindow.z)) + vWindow.xy;
        index = texture(uIndex, (texel0 + 0.5) / uTexSize).r * 255.0;
        vec2 palUv = vec2((index + 0.5) / uPaletteSize, (vPalette + 0.5) / uPaletteCount);
        vec4 texel = texture(uPalette, palUv);
        // PS1 convention: a palette entry of 0x0000 is transparent. uDebug 5 keeps those fragments
        // so a check can count how much of a surface the hardware throws away.
        if (texel.a < 0.5 && uDebug != 5) discard;
        base = texel.rgb;
        if (modulate) base = min(floor(base * 255.0 + 0.5) * shade8 / 128.0, vec3(255.0)) / 255.0;
    }
    // A group that carries no normal array is UNLIT. `DOD3+0x28` (the normal array) and `+0x30`
    // (the vertex -> normal index) are both 0 in all 39 groups of HAZARD0.GEO, so every prop
    // vertex arrives here with a zero normal, and `normalize()` of a zero vector is NaN. A NaN
    // colour is written to the framebuffer as 0 - which is why the props came out pure black,
    // textured and untextured alike. No normal means no lighting term, not a NaN one.
    //
    // The bike and the rider are the opposite case: `BBLEVEL1.GEO` model 100 group 0 carries 48
    // normals and model 150 group 0 carries 49, so they ARE lit and their pixel is not the palette
    // entry. `shade` is therefore quantised to the same 1/255 grid the framebuffer uses and
    // reported by uDebug 3, so the per-pixel check can predict `round(palette * shade)` exactly
    // instead of having to guess a lighting term.
    float shade = 1.0;
    float nLength = length(vNormal);
    if (nLength > 0.0) {
        // Two-sided lambert: the original data has no consistent winding for us to trust yet.
        float lambert = abs(dot(vNormal / nLength, normalize(vec3(0.35, 0.8, 0.5))));
        shade = 0.25 + 0.75 * lambert;
    }
    shade = floor(shade * 255.0 + 0.5) / 255.0;
    if (uDebug != 0) {
        // Same coverage as the real draw (same discard), but reporting what was sampled rather
        // than what it looks like, so a script can check the frame against the binding rule.
        // Blue 255 marks the fragment as a reported one; no lit surface in the scene reaches it.
        if (uDebug == 1) oColor = vec4(vPalette / 255.0, index / 255.0, 1.0, 1.0);
        else if (uDebug == 2) oColor = vec4(uDebugId / 255.0, 0.0, 1.0, 1.0);
        else if (uDebug == 3) oColor = vec4(shade, 0.0, 1.0, 1.0);
        else if (uDebug == 5) oColor = vec4(0.0, 0.0, 1.0, 1.0); // coverage, discard suppressed
        // 6 is the silhouette: EVERY surface, with the real discard, so a pixel that is not blue
        // is a pixel the frame really leaves empty. It is what a "hole" is measured against, and
        // it needs no guess about what the clear colour rounds to in 8 bits.
        else if (uDebug == 6) oColor = vec4(0.0, 0.0, 1.0, 1.0);
        // 7 and 8 report the vertex colour a modulated fragment was multiplied by, (r, g) and
        // (b, mode), so the cell check can predict `palette * colour / 128` channel by channel.
        else if (uDebug == 7) oColor = vec4(shade8.r / 255.0, shade8.g / 255.0, 1.0, 1.0);
        else if (uDebug == 8) oColor = vec4(shade8.b / 255.0, shadeMode / 255.0, 1.0, 1.0);
        else oColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    oColor = vec4(base * shade, 1.0);
}
)";

// ---------------------------------------------------------------- the panorama (type-4 chunk)
// Its own tiny program: the backdrop is a direct-colour image, not an indexed one, so it shares
// nothing with the shader above except the view-projection matrix.
const char* const kSkyVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexel;    // absolute texel coordinates in the 1760 x 128 panorama
uniform mat4 uViewProj;
// The world sky (DrawRequest::worldSky, a stereo eye / the head camera; race_scene.cpp RebuildSkyBand): the continuous
// band, aPos a DIRECTION - w = 0 drops every translation, so the backdrop is at infinity (rotation only, the same in
// both eyes), and z sits on the far plane.
uniform int uWorld;
out vec2 vTexel;
void main() {
    vTexel = aTexel;
    if (uWorld != 0) {
        gl_Position = uViewProj * vec4(aPos, 0.0);
        gl_Position.z = gl_Position.w * 0.999999;
    } else {
        gl_Position = uViewProj * vec4(aPos, 1.0);
    }
}
)";

const char* const kSkyFragmentShader = R"(#version 330 core
in vec2 vTexel;
uniform sampler2D uSky;
uniform vec2 uSkySize;
// The world sky: the band (RebuildSkyBand), sampled through the texture's own filter (nearest or bilinear). Its empty
// and cut-out texels are (0, 0, 0, 0), so the filtered colour over the filtered alpha is the opaque neighbours' mix.
uniform int uWorld;
// 0 draws the backdrop; 1 reports the texel column it sampled as (high byte, low byte); 2 reports
// the texel row; 4 means "not the subject of the check" - same coverage, blue 0.
uniform int uDebug;
out vec4 oColor;
void main() {
    if (uWorld != 0) {
        vec4 b = texture(uSky, vTexel / uSkySize);
        if (b.a < 0.5) discard;
        oColor = vec4(b.rgb / b.a, 1.0);
        return;
    }
    float tu = floor(vTexel.x);
    tu = tu - uSkySize.x * floor(tu / uSkySize.x);          // the panorama is a closed cylinder
    float tv = clamp(floor(vTexel.y), 0.0, uSkySize.y - 1.0);
    vec4 texel = texture(uSky, vec2((tu + 0.5) / uSkySize.x, (tv + 0.5) / uSkySize.y));
    // A band STEN leaves empty carries no tile at all, so it is transparent and the frame shows
    // whatever is behind it - which above the skyline is nothing.
    if (texel.a < 0.5) discard;
    if (uDebug == 1) {
        float hi = floor(tu / 256.0);
        oColor = vec4(hi / 255.0, (tu - 256.0 * hi) / 255.0, 1.0, 1.0);
    } else if (uDebug == 2) {
        oColor = vec4(tv / 255.0, 0.0, 1.0, 1.0);
    } else if (uDebug != 0) {
        oColor = vec4(0.0, 0.0, 0.0, 1.0);
    } else {
        oColor = vec4(texel.rgb, 1.0);
    }
}
)";

// ---------------------------------------------------------------- the sky gradient
// The layer the type-4 panorama is NOT: the Gouraud sky the original paints before anything else
// (`RASHCDG 0x80063C5C`, emitter `0x800642F8`). It is a screen-space quad strip, so it needs no
// view matrix at all - the vertices arrive already in NDC.
//
// `vRow` runs 0 at the top of the screen to 1 at the horizon and `vSide` 0 at the left edge to 1 at
// the right, and the colour is built from them in the shader, because that is what lets a check
// predict the pixel: the two parameters are quantised to the same 1/255 grid the framebuffer uses
// and reported, so the CPU can evaluate the identical function on the identical inputs.
const char* const kGradVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;      // normalised device coordinates
layout(location = 1) in vec2 aParam;    // (row, side)
out vec2 vParam;
void main() {
    vParam = aParam;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* const kGradFragmentShader = R"(#version 330 core
in vec2 vParam;
uniform vec3 uTop;      // entry A of the level bundle: the colour at the top of the sky
uniform vec3 uHorizon;  // entry B: the colour at the horizon
uniform vec3 uMidLeft;  // the blend of entries C and D at the left screen edge
uniform vec3 uMidRight; // the same blend at the right screen edge
// 0 draws the gradient; 1 reports the two interpolation parameters; 4 means "not the subject".
uniform int uDebug;
out vec4 oColor;
void main() {
    float row = clamp(floor(vParam.x * 255.0 + 0.5) / 255.0, 0.0, 1.0);
    float side = clamp(floor(vParam.y * 255.0 + 0.5) / 255.0, 0.0, 1.0);
    if (uDebug == 1) {
        oColor = vec4(row, side, 1.0, 1.0);
        return;
    }
    if (uDebug != 0) {
        oColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 mid = mix(uMidLeft, uMidRight, side);
    vec3 c = row < 0.5 ? mix(uTop, mid, row * 2.0) : mix(mid, uHorizon, (row - 0.5) * 2.0);
    oColor = vec4(c, 1.0);
}
)";

// ---------------------------------------------------------------- the cloud ring
// `RASHCDG 0x8006396C` emits each segment as a POLY_FT4 with code 0x2F: raw texture (no
// modulation), semi-transparent. The GPU interpolates UVs affinely in screen space, per triangle,
// so the texel coordinate is `noperspective`. The image's alpha carries the STP rule: 0 for the
// transparent entry 0x0000, 128 for an entry with bit 15 set (blended B/2 + F/2, tpage mode 0),
// 255 for an opaque one.
const char* const kCloudVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexel;    // texel coordinates in the 256 x 63 cloud image
uniform mat4 uViewProj;
// The world sky (DrawRequest::worldSky, a stereo eye / the head camera): aPos is a DIRECTION (w = 0: at infinity, no
// translation, the same in both eyes), z on the far plane, and the texels perspective-correct - the GPU's affine
// mapping of a 15-degree segment swims as the head turns (a screen-space warp the console's fixed camera never shows).
uniform int uWorld;
noperspective out vec2 vTexel;
centroid out vec2 vTexelWorld;
void main() {
    vTexel = aTexel;
    vTexelWorld = aTexel;
    if (uWorld != 0) {
        gl_Position = uViewProj * vec4(aPos, 0.0);
        gl_Position.z = gl_Position.w * 0.999999;
    } else {
        gl_Position = uViewProj * vec4(aPos, 1.0);
    }
}
)";

const char* const kCloudFragmentShader = R"(#version 330 core
noperspective in vec2 vTexel;
centroid in vec2 vTexelWorld;
uniform sampler2D uClouds;
uniform int uWorld;
uniform vec4 uRect; // the world sky: this segment's slice u0 u1 v0 v1 (a texel of its own slice only)
// 0 draws the layer; anything else means "not the subject": same coverage, opaque, blue 0.
uniform int uDebug;
out vec4 oColor;
void main() {
    ivec2 size = textureSize(uClouds, 0);
    ivec2 t = clamp(ivec2(floor(vTexel)), ivec2(0), size - ivec2(1));
    if (uWorld != 0)
        t = clamp(ivec2(clamp(floor(vTexelWorld), uRect.xz, uRect.yw)), ivec2(0), size - ivec2(1));
    vec4 texel = texelFetch(uClouds, t, 0);
    if (texel.a < 0.25) discard;
    if (uDebug != 0) {
        oColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    oColor = vec4(texel.rgb, texel.a > 0.75 ? 1.0 : 0.5);
}
)";

// ---------------------------------------------------------------- the bike's shadow
// `SLUS 0x80025EE0`: every quad of a model's `quadsD` list, projected onto the ground plane along
// the level's light vector, emitted as a flat semi-transparent F4 (code 0x2A) under draw mode
// 0xE1000740 (semi-transparency mode 2, B - F) between `E6 3` and `E6 0` (set and test the mask
// bit), i.e. every covered pixel is darkened by the colour at 0x80052348 exactly once. The stencil
// buffer plays the mask bit and the blend equation is reverse-subtract.
const char* const kShadowVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;      // world position, already on the ground plane
uniform mat4 uViewProj;
uniform int uOtOrder;   // race_scene.h SetOtOrder: the quad's slot depth instead of its own
uniform float uOtDepth; // 0..1
// The console's fill rule (edge_rule.h): the packet's SXY and z = 1 when the corner is to sit exactly there (uGteMap / uGtePix
// as the main program's) and take the console's fill rule; a vertex array without attribute 1 reads 0: the world point.
layout(location = 1) in vec3 aSxy;
uniform vec4 uGteMap;
uniform float uGtePix;
out vec3 eCon;
void main() {
    vec4 clip = uViewProj * vec4(aPos, 1.0);
    eCon = vec3(aSxy.xy, 0.0);
    if (aSxy.z > 0.5 && clip.w > 0.0) {
        clip.xy = vec2((aSxy.x + uGtePix) * uGteMap.x + uGteMap.y, (aSxy.y + uGtePix) * uGteMap.z + uGteMap.w) * clip.w;
        eCon.z = 1.0;
    }
    gl_ClipDistance[0] = clip.z + clip.w;
    gl_ClipDistance[1] = clip.w - clip.z;
    if (uOtOrder != 0) clip.z = (uOtDepth * 2.0 - 1.0) * clip.w;
    gl_Position = clip;
}
)";

const char* const kShadowFragmentShader = R"(#version 330 core
uniform vec3 uColour;
uniform int uDebug;
out vec4 oColor;
void main() {
    if (uDebug != 0) {
        oColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    oColor = vec4(uColour, 1.0);
}
)";

// ---------------------------------------------------------------- the PS1 look
// A full-screen triangle over a copy of the finished frame. The GPU writes 15-bit colour; with
// dithering on (E1 bit 9, set by the game for its shaded and blended polygons) it adds the 4x4
// ordered-dither offset below to each 8-bit channel before dropping the low three bits. The pattern
// is laid on the CONSOLE's pixel grid (384 x 240 scaled to the viewport), so it looks as coarse as
// it did on a television rather than vanishing at the window's resolution.
const char* const kPostVertexShader = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

const char* const kPostFragmentShader = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uFrame;
uniform int uDither;
uniform vec2 uOrigin;     // the viewport's lower-left corner in window pixels
uniform float uScale;     // window pixels per console pixel
out vec4 oColor;
const float kDither[16] = float[16](-4.0, 0.0, -3.0, 1.0,
                                     2.0, -2.0, 3.0, -1.0,
                                    -3.0, 1.0, -4.0, 0.0,
                                     3.0, -1.0, 2.0, -2.0);
void main() {
    vec3 c = floor(texture(uFrame, vUv).rgb * 255.0 + 0.5);
    if (uDither != 0) {
        // Console rows run top to bottom, GL's bottom to top.
        vec2 console = floor((gl_FragCoord.xy - uOrigin) / uScale);
        int x = int(mod(console.x, 4.0));
        int y = int(mod(-console.y - 1.0, 4.0));
        c = clamp(c + kDither[y * 4 + x], 0.0, 255.0);
    }
    vec3 five = floor(c / 8.0);
    oColor = vec4((five * 8.0 + floor(five / 4.0)) / 255.0, 1.0);
}
)";

// ---------------------------------------------------------------- the HUD
// Screen-space quads in normalised device coordinates, sampling the already-decoded `DASH?P.TEX`
// page as straight RGBA. The page is 4bpp on the console and its palettes are its own CLUT rows,
// but `rr::DecodeDashTextures` has already resolved that on the CPU, so nothing here selects a
// palette. `uTint` with `uTextured == 0` draws a flat rectangle, which is what the simulation
// readout bars use.
const char* const kHudVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;    // normalised device coordinates
layout(location = 1) in vec2 aUv;     // 0..1 over the atlas
out vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* const kHudFragmentShader = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uPage;
uniform int uTextured;
uniform vec4 uTint;
out vec4 oColor;
void main() {
    if (uTextured == 0) {
        oColor = uTint;
        return;
    }
    vec4 texel = texture(uPage, vUv);
    if (texel.a < 0.5) discard;
    oColor = vec4(texel.rgb * uTint.rgb, 1.0);
}
)";

std::string EsShaderSource(GLenum type, const char* source, const GlCaps& caps) {
    std::string s = source;
    const std::string desktop = "#version 330 core";
    if (s.compare(0, desktop.size(), desktop) != 0)
        throw std::runtime_error("EsShaderSource: the shader does not start with '#version 330 core'");
    std::string head = "#version 320 es\n";
    // #extension lines must precede every other token of the shader
    if (caps.clipDistance) head += "#extension GL_EXT_clip_cull_distance : enable\n";
    if (caps.noPerspective) head += "#extension GL_NV_shader_noperspective_interpolation : enable\n";
    // ES has no default float precision in the fragment stage and none at all for samplerBuffer; highp everywhere
    // keeps the integer and float arithmetic of the desktop shaders (32-bit) unchanged.
    head += "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp samplerBuffer;\n";
    if (!caps.noPerspective) head += "#define noperspective\n"; // interpolated perspective-correct instead
    if (!caps.clipDistance && (type == GL_VERTEX_SHADER))       // written, never read: the planes are simply not applied
        head += "float rrClipDistance[8];\n#define gl_ClipDistance rrClipDistance\n";
    return head + s.substr(desktop.size() + (s.size() > desktop.size() && s[desktop.size()] == '\n' ? 1 : 0));
}

GLuint CompileShader(GLenum type, const char* source) {
    const GLuint shader = gl.CreateShader(type);
    // single-pass stereo (multiview.h): a world program compiled while the VR host draws both eyes in one pass
    const bool multiview = type == GL_VERTEX_SHADER && MultiviewPrograms() && WantsMultiview(source);
#ifdef RR_GLES
    std::string esSource = EsShaderSource(type, source, glCaps);
    if (multiview) esSource = MultiviewVertexSource(esSource);
    source = esSource.c_str();
#else
    std::string mvSource;
    if (multiview) {
        mvSource = MultiviewVertexSource(source);
        source = mvSource.c_str();
    }
#endif
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        gl.GetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        throw std::runtime_error(std::string("shader compile failed: ") + log);
    }
    return shader;
}

GLuint BuildProgram(const char* vertexSource, const char* fragmentSource) {
    const GLuint program = gl.CreateProgram();
    gl.AttachShader(program, CompileShader(GL_VERTEX_SHADER, vertexSource));
    gl.AttachShader(program, CompileShader(GL_FRAGMENT_SHADER, fragmentSource));
    gl.LinkProgram(program);
    GLint ok = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        gl.GetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        throw std::runtime_error(std::string("program link failed: ") + log);
    }
    return program;
}

} // namespace rr::render
