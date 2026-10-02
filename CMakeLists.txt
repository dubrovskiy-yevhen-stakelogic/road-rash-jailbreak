cmake_minimum_required(VERSION 3.24)
project(rrjb VERSION 0.1.0 LANGUAGES CXX) # the version scripts\package-source.ps1 / package-player.ps1 stamp

# Player executables carry the C++ runtime, so a clean Windows PC needs no separate VC runtime install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX /permissive- /utf-8 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

add_library(rrvfs STATIC
    src/rrvfs/disc_image.cpp
    src/rrvfs/sha1.cpp          # disc identification (rrtool identify, the installer)
    src/rrvfs/disc_identity.cpp # supported-release check and rrgame's disc lookup (disc.txt, runtime\disc)
)
target_include_directories(rrvfs PUBLIC src)

add_library(rrformats STATIC
    src/rrformats/rmd3.cpp
    src/rrformats/road.cpp
    src/rrformats/skeleton.cpp
    src/rrformats/chunk.cpp
    src/rrformats/texture.cpp
    src/rrformats/cell.cpp
    src/rrformats/dash.cpp
    src/rrformats/model_texture.cpp
    src/rrformats/audio.cpp
    src/rrformats/camera_ca.cpp
    src/rrformats/mdec.cpp
    src/rrformats/hd_pack.cpp # the optional HD media pack (docs/HD-MEDIA.md), stb_image / stb_image_write (third_party/stb)
)
target_include_directories(rrformats PUBLIC src)

# Development oracle only - the R3000A + GTE interpreter is never linked into the shipped game.
add_library(rrinterp STATIC
    src/interp/r3000.cpp
    src/interp/gte.cpp
    src/interp/snapshot.cpp
    src/interp/trace.cpp
    src/interp/devices.cpp
)
target_include_directories(rrinterp PUBLIC src)
target_link_libraries(rrinterp PUBLIC rrformats) # the MDEC device: rr::mdec

add_library(rrplatform STATIC
    src/platform/png.cpp
    src/platform/audio_device_winmm.cpp
    src/platform/input_bindings.cpp # the rebindable controls -> the original's pad word
    src/platform/pad_decode.cpp # XInput / WinMM state -> the physical pad
    src/platform/dualsense.cpp # the DualSense over HID: input, rumble, adaptive triggers
    src/platform/dualsense_report.cpp
)
target_include_directories(rrplatform PUBLIC src)
if(WIN32)
    target_link_libraries(rrplatform PUBLIC winmm hid setupapi)
endif()

# The software mixer. Standard library only on purpose: headless tests and the cross-check tools mix
# without touching an audio device.
add_library(rrgameaudio STATIC
    src/game/audio/mixer.cpp
    src/game/audio/sound_runtime.cpp
    src/game/audio/sound_loader.cpp # the sound set-up PORTED
)
target_include_directories(rrgameaudio PUBLIC src)
# The sound runtime reads the player's own DATA\RASHNZ_E.DAT through the proven ADPCM codec and runs
# the PORTED emitter of src/game/sim/sound.cpp on it.
target_link_libraries(rrgameaudio PUBLIC rrformats rrgamesim)

# Native gameplay layer (the product side). Never links the interpreter.
add_library(rrgamecore STATIC
    src/game/world.cpp
    src/game/race_session.cpp
    src/game/parity_session.cpp # rrgame --parity: a capture as the arena
    src/game/fight_session.cpp
    src/game/fight_physical.cpp # VR physical combat: a contact through the ported fight code
    src/game/speech_session.cpp # the riders' voices
    src/game/weapon_session.cpp # the weapons
    src/game/cheats.cpp # the cheat menu
    src/game/weapon_inventory.cpp # the player's weapons as the record keeps them
    src/game/handling_modern.cpp # Original / Modern (SA-style) handling: the input and the drawn lean
    src/game/wheelie.cpp # wheelies: the lean-back input, the drawn pitch, the car hit's alternative outcome
    src/game/traffic_arena.cpp
    src/game/coll_product.cpp
    src/game/pad_product.cpp # the pad reader in the game
    src/game/bike_pose_product.cpp # the bike's drawn pose in the game
    src/game/ai_race.cpp
    src/game/cop_race.cpp # the police chase and the arrest
    src/game/race_modes.cpp # the game modes: the loader's per-race-type setup, the player cop (rules.md 9)
    src/game/jail_session.cpp # the Jailbreak mode and the two-seat bike in the product (rules.md 16)
    src/game/recover_race.cpp
    src/game/grid_loader.cpp
    src/game/grid_session.cpp # the ported BuildGrid on the arena
    src/game/loader_product.cpp # the race loader's set-up path on the arena
    src/game/loader2_cam_product.cpp # loader2_cam: the camera set-up / light stores / collision set-up ports in the game
    src/game/route_product.cpp # GrfLoad / RoadLoad / StreamSetUp / StreamStart PORTED in the game
    src/game/stream_files_product.cpp # the stream's files and the .STP start PORTED in the game
    src/game/stream_session.cpp # the original's streamer in the product
    src/game/frame_ot.cpp # the frame's ordering tables where SLUS 0x8001C1AC puts them
    src/game/hud_arena.cpp
    src/game/hud_view.cpp
    src/game/hud_hd.cpp # the HUD at the HD pack's scale (docs/HD-MEDIA.md)
    src/game/rider_pose.cpp
    src/game/anim_detail.cpp # maximum detail: the full animation at every distance, render-only
    src/game/head_camera.cpp # the head camera: first person on the player rider's head
    src/game/fx_runtime.cpp # the effects
    src/game/model_runtime.cpp # the model draw
    src/game/shadow_product.cpp # the model shadow in the product
    src/game/mp2_product.cpp # two players: the remaining pieces in the product
    src/game/mp_session.cpp # two players
    src/game/rider_model.cpp # the rider model class and box
    src/game/cell_view.cpp # the cells each view draws (scene_cell.md 14)
    src/game/cell_sort_product.cpp # the frame's cell sort in the product
    src/game/world_pop_product.cpp # the cell walker, props, collision volumes
    src/game/hazard_product.cpp # the hazard objects: set-up, spawner, pass, draw
    src/game/peds_product.cpp # the pedestrians of pool 2
    src/game/solid_product.cpp # poles, props
    src/game/animobj_product.cpp # the original's animation objects
    src/game/junction_product.cpp # SLUS 0x8003E338 in the rider pass's pool loop
    src/game/takedown_product.cpp # the takedowns
    src/game/strike_product.cpp # op 9's strike, PedVoice, RaceOverSignal
    src/game/pause_product.cpp # the pause test, the stall switch, the pause menu
    src/game/passes_product.cpp # the top-level passes whole
    src/game/sky_product.cpp # the sky draw, the host MDEC / VRAM
    src/game/rumble_product.cpp # the pad motors in the game
)
target_include_directories(rrgamecore PUBLIC src)
target_link_libraries(rrgamecore PUBLIC rrvfs rrformats rrgamesim rrgameaudio)

add_executable(rrtool tools/rrtool/main.cpp)
target_link_libraries(rrtool PRIVATE rrvfs rrformats rrgamecore rrplatform)

# Decode and play the game's audio: `decode` writes the same WAV names as tools\scout\audio.py, which
# is what the sample-for-sample cross-check between the two decoders diffs.
add_executable(rraudio tools/rraudio/main.cpp)
target_link_libraries(rraudio PRIVATE rrformats rrgameaudio rrplatform)

# The renderer itself: window, GL, shaders and the scene the game and the instrument BOTH draw.
# `rrview` proves this code per pixel against the palette the binding rules predict; `rrgame` plays
# it. One copy, so the checks are about the game's own frame.
add_library(rrrender STATIC
    src/render/gl_api.cpp
    src/render/mat4.cpp
    src/render/shaders.cpp
    src/render/gpu_texture.cpp
    src/render/window_win32.cpp
    src/render/frame_shot.cpp # ReadFrame / SaveShot, portable (the desktop window, the VR eyes, the Quest)
    src/render/scene_geometry.cpp
    src/render/race_scene.cpp
    src/render/race_scene_sidecar.cpp # the two-seat machine (rules.md 16)
    src/render/race_scene_sidecar2.cpp # player 2's own rig
    src/render/race_scene_rivals.cpp # the other riders' own models and palettes
    src/render/race_scene_shadow.cpp # the ported model shadow drawn
    src/render/race_scene_ot.cpp # the scene in the original's ordering-table order
    src/render/gte_proj.cpp # the world's vertices at the GTE's own SXY
    src/render/traffic_draw.cpp
    src/render/ped_draw.cpp # the pedestrians, posed
    src/render/rider_pose_draw.cpp
    src/render/lod_pose_draw.cpp # LODs 1..3 of the bike and the rider
    src/render/fx_draw.cpp # the effects
    src/render/sky_gpu.cpp # the sky packets drawn as the GPU draws them
    src/render/edge_rule.cpp # the console's fill rule on GL
    src/render/weapon_draw.cpp # the weapons
)
target_include_directories(rrrender PUBLIC src)
target_link_libraries(rrrender PUBLIC rrvfs rrformats rrplatform rrgamesim opengl32) # rrgamesim: ApproxLen3

# THE PRODUCT. It links the renderer, the asset parsers and the ported gameplay - and never the
# interpreter, which is a development oracle only.
add_executable(rrgame tools/rrgame/main.cpp)
target_link_libraries(rrgame PRIVATE rrvfs rrformats rrgamecore rrgamesim rrgameaudio rrplatform
                                     rrrender opengl32)

# Console subsystem on purpose: the viewer prints what it parsed before it opens a window.
add_executable(rrview tools/rrview/main.cpp)
target_link_libraries(rrview PRIVATE rrvfs rrformats rrgamecore rrgameaudio rrplatform rrrender opengl32)

# The bike simulation: gameplay ported function by function from the original MIPS code and
# accepted only by `rrverify phys`. Integer-exact fixed point, no floats, no dependency on the
# interpreter or on any asset parser.
add_library(rrgamesim STATIC
    src/game/sim/fixed.cpp
    src/game/sim/vec.cpp
    src/game/sim/bike.cpp
    src/game/sim/ai.cpp
    src/game/sim/ai_brain.cpp
    src/game/sim/ai_globals.cpp src/game/sim/ai_cmd.cpp
    src/game/sim/road.cpp
    src/game/sim/road_query.cpp
    src/game/sim/road_runtime.cpp
    src/game/sim/input.cpp
    src/game/sim/race.cpp src/game/sim/spine.cpp
    src/game/sim/sound.cpp
    src/game/sim/sound_engine.cpp
    src/game/sim/sound_frame.cpp
    src/game/sim/speech.cpp # the riders' voices
    src/game/sim/integrator.cpp
    src/game/sim/bike_step.cpp
    src/game/sim/crash.cpp
    src/game/sim/ground.cpp
    src/game/sim/anim.cpp src/game/sim/stance.cpp src/game/sim/pose.cpp src/game/sim/present.cpp
    src/game/sim/population.cpp
    src/game/sim/ai_plan.cpp
    src/game/sim/traffic.cpp src/game/sim/traffic_leaves.cpp src/game/sim/traffic_bind.cpp
    src/game/sim/traffic_drive.cpp src/game/sim/police.cpp
    src/game/sim/cops.cpp # the police chase and the arrest
    src/game/sim/modes.cpp # the game modes: player cop, Jailbreak (rules.md 9)
    src/game/sim/jail.cpp # the Jailbreak escape scene (rules.md 16)
    src/game/sim/camera.cpp
    src/game/sim/collision.cpp
    src/game/sim/coll_serve.cpp src/game/sim/contact.cpp src/game/sim/impact_solve.cpp
    src/game/sim/hit_speed.cpp src/game/sim/bike_react.cpp src/game/sim/resolvers.cpp
    src/game/sim/solid.cpp # poles, props, hazards
    src/game/sim/partners.cpp # rider/traffic, rider/shape, traffic/traffic
    src/game/sim/junction.cpp # SLUS 0x8003E338 with NodeWedge native
    src/game/sim/hud.cpp
    src/game/sim/fight.cpp
    src/game/sim/strike.cpp src/game/sim/race_over.cpp # the strike: 0x800C1370, SLUS 0x80018C1C
    src/game/sim/pause.cpp # the pause test, the stall switch, the pause menu
    src/game/sim/takedown.cpp # the knock-off credit, rider-off sound, busted music
    src/game/sim/passes.cpp # the world / rider-engine passes and the rider pass whole
    src/game/sim/weapon.cpp # the weapons
    src/game/sim/split_view.cpp # two players
    src/game/sim/effects.cpp # the effects
    src/game/sim/fx_glow.cpp # the emitter's glow sprites
    src/game/sim/anim_objects.cpp # the animation objects' initialiser and hand-out
    src/game/sim/model_draw.cpp # the model draw
    src/game/sim/cell_draw.cpp # which cells a view draws (scene_cell.md 14)
    src/game/sim/cell_sort.cpp # the frame's cell sort, the OT set-up
    src/game/sim/subdiv.cpp # the cell draw's polygon subdividers (scene_cell.md 13.8)
    src/game/sim/view_pass.cpp # GameFrame step 4 (view distances, LODs)
    src/game/sim/world_pop.cpp # the cell walker, props, collision volumes
    src/game/sim/peds.cpp # the pedestrians of pool 2
    src/game/sim/hazard.cpp # the hazard objects: keyframed flyers over the road
    src/game/sim/stream.cpp src/game/sim/stream_load.cpp # the original's streamer
    src/game/sim/stream_cd.cpp # the CD layer, the sky pick, the cell textures
    src/game/sim/sky_draw.cpp # the panorama and cloud packets, the MDEC column decode
    src/game/sim/sky_split.cpp # the gradient, the two-player sky, the packet heap manager
    src/game/sim/shadow.cpp # the model shadow SLUS 0x80025EE0
    src/game/sim/shadow2p.cpp # two players: the shadow SLUS 0x80026960
    src/game/sim/mp_world.cpp # two players: the cell copy 0x8001339C, the class-50 release 0x800A3ECC
    src/game/sim/recover.cpp src/game/sim/recover_walk.cpp src/game/sim/recover_fall.cpp
    src/game/sim/recover_ground.cpp src/game/sim/recover_air.cpp
    src/game/sim/rider_record.cpp
    src/game/sim/grid_build.cpp # the race loader's starting grid
    src/game/sim/loader.cpp # the race loader's set-up path
    src/game/sim/loader2.cpp # BuildRace and SetUpRace's children
    src/game/sim/route_parse.cpp # the route block parser, the road-map loader
    src/game/sim/stream_files.cpp # the stream's files, the .STP start, the CD waits, the album
    src/game/sim/camera_setup.cpp # loader2_cam: the camera set-up, the light stores, the collision set-up
    src/game/sim/spu_heap.cpp # libspu's SPU-RAM allocator, the music rings' buffers
    src/game/sim/camera_collide.cpp src/game/sim/pad_reader.cpp src/game/sim/ped_hit.cpp src/game/sim/camera_director.cpp src/game/sim/bike_parts.cpp # camera collision, pad reader, ped hits, camera director, bike parts
    src/game/sim/pad_motor.cpp src/game/sim/countdown_voice.cpp # the pad motors, the countdown voice
)
target_include_directories(rrgamesim PUBLIC src)

# The per-function bit-exact bench: runs one original guest function inside rrinterp and diffs the
# whole guest RAM outside the guest stack, plus the scratchpad, against our native C++.
add_executable(rrverify tools/rrverify/main.cpp tools/rrverify/gte_check.cpp tools/rrverify/trace_cmd.cpp tools/rrverify/texbind_cmd.cpp tools/rrverify/cellbind_cmd.cpp
    tools/rrverify/verify_physics.cpp tools/rrverify/menu_cmd.cpp tools/rrverify/mdec_cmd.cpp)
target_link_libraries(rrverify PRIVATE rrinterp rrvfs rrformats rrgamesim rrplatform)

# The front end: the shell's ported logic on its own guest arena,
# its screens drawn from the parsed widget data, the handover to the race and back, the card file.
# `rrshell` holds the development checks that compare it with the original.
add_library(rrgameshell STATIC
    src/game/shell/shell_logic.cpp
    src/game/shell/shell_arena.cpp
    src/game/shell/shell_assets.cpp
    src/game/shell/shell_view.cpp
    src/game/shell/front_end.cpp
    src/game/shell/movie_player.cpp # the films
    src/game/shell/shell_card.cpp
    src/game/shell/shell_memcard.cpp # the card screens 43..46
    src/game/shell/shell_screens.cpp
    src/game/shell/shell_text.cpp
    src/game/shell/race_results.cpp
    src/game/shell/shell_sound.cpp
    src/game/shell/shell_career.cpp
    src/game/shell/shell_widgets.cpp # the widget draw handlers
    src/game/shell/shell_panel.cpp # the panel films and logos, widget types 2..6 and 8
    src/game/shell/panel_films.cpp # the product's film library for them (OURS)
)
target_include_directories(rrgameshell PUBLIC src)
target_link_libraries(rrgameshell PUBLIC rrvfs rrformats rrgamesim rrgameaudio)
add_executable(rrshell tools/rrshell/main.cpp)
target_link_libraries(rrshell PRIVATE rrgameshell rrvfs rrformats rrplatform)
# The OFFLINE HD media preparation (docs/HD-MEDIA.md, scripts/prepare-hd.ps1): originals out of the player's disc, the
# contour atlases, the neural pictures and films packed with their source hashes. Never run by the game.
add_executable(rrhd tools/rrhd/main.cpp)
target_link_libraries(rrhd PRIVATE rrgameshell rrgamecore rrvfs rrformats rrgamesim)
target_link_libraries(rrverify PRIVATE rrgameshell)
# verify_physics.cpp includes every rows_*.inc and passed MSVC's default COFF section limit.
if(MSVC)
    target_compile_options(rrverify PRIVATE /bigobj)
endif()
# rrgame without --race opens the front end first (tools\rrgame\front_end_main.cpp).
target_sources(rrgame PRIVATE tools/rrgame/front_end_main.cpp)
target_sources(rrgame PRIVATE tools/rrgame/fx_check.cpp) # --fxcheck
target_sources(rrgame PRIVATE tools/rrgame/model_check.cpp) # --modelplant / --modelfxcheck
target_sources(rrgame PRIVATE tools/rrgame/parity.cpp) # --parity: the product's frame of a capture
target_sources(rrgame PRIVATE tools/rrgame/padmap_check.cpp) # --padmapcheck / --dualsenseprobe
target_link_libraries(rrgame PRIVATE rrgameshell)
# The graphics settings and the F10 overlay, the per-view renderer
# (race_render.h, the VR-ready entry), the offscreen target / MSAA / VSync, smooth textures, maximum detail.
target_sources(rrgame PRIVATE tools/rrgame/cheat_menu.cpp) # the cheat menu, its settings, the career cheats
target_sources(rrgame PRIVATE tools/rrgame/hd_media.cpp) # the HD media pack: its folder, the switch, the HD HUD (docs/HD-MEDIA.md)
target_sources(rrgame PRIVATE tools/rrgame/handling_settings.cpp) # the handling setting, its flags and menu rows
target_sources(rrgame PRIVATE tools/rrgame/handling_view.cpp) # the head view's drawn lean and view roll meter
target_sources(rrgame PRIVATE tools/rrgame/vr_wheelie.cpp) # the wheelie's drawn pitch, VR view comfort, the bars' gesture

target_sources(rrgame PRIVATE tools/rrgame/race_render.cpp tools/rrgame/graphics_settings.cpp tools/rrgame/pc_overlay.cpp
                              tools/rrgame/zfight_probe.cpp) # DEVELOPMENT: RRJB_ZFIGHT, the depth-fight detector
target_sources(rrgame PRIVATE tools/rrgame/carflick_probe.cpp) # DEVELOPMENT: RRJB_CARFLICK, the distant cars' brightness
# VR: the portable loops over a host (game_host.h) - the desktop window,
# or the VR host (OpenXR / the desktop VR mock) - and the VR frame's pieces. main_desktop.cpp is Windows' entry;
# the Quest's is main_android.cpp (cmake/vr.cmake builds librrgame.so from the same list).
set(RR_GAME_PORTABLE_SOURCES tools/rrgame/game_host.cpp tools/rrgame/game_host_vr.cpp tools/rrgame/game_host_vr_xr.cpp
    tools/rrgame/vr_rig.cpp tools/rrgame/vr_settings.cpp tools/rrgame/vr_menu.cpp tools/rrgame/vr_draw.cpp
    tools/rrgame/settings_file.cpp # rrgame_settings.ini's sections: one reader, one writer
    tools/rrgame/settings_check.cpp) # --settings-roundtrip-check
target_sources(rrgame PRIVATE ${RR_GAME_PORTABLE_SOURCES} tools/rrgame/main_desktop.cpp tools/rrgame/game_host_desktop.cpp)
# VR steering with the hands on the handlebars: the settings, the grips from the rider's gloves, the
# hands' solver (grab, bars, twist throttle, swings, the mock's script), the procedural gloves. Portable (the Quest too).
target_sources(rrgame PRIVATE tools/rrgame/vr_bars_settings.cpp tools/rrgame/vr_bar_grips.cpp tools/rrgame/vr_handlebars.cpp
                              tools/rrgame/vr_hands_draw.cpp
                              src/platform/png_read.cpp) # the hands' albedo (png_read.h)
include(cmake/vr-hand-assets.cmake) # the gt2-play / UltimateXR hands embedded (third_party/vrhands)
# VR physical combat: the tracked fists / weapon swept against the riders' posed bodies, the settings.
target_sources(rrgame PRIVATE tools/rrgame/vr_melee.cpp tools/rrgame/vr_melee_settings.cpp)
# VR holsters, the weapons in the hands, the grip calibration and the swing's rules.
target_sources(rrgame PRIVATE tools/rrgame/vr_holsters.cpp tools/rrgame/vr_weapon_calib.cpp tools/rrgame/vr_weapon_settings.cpp)
# VR weapon snatching and the nunchaku's chain: a free hand takes a rival's swinging weapon through
# WeaponSteal, the jointed weapons swing on a physics chain in the hand.
target_sources(rrgame PRIVATE tools/rrgame/vr_snatch.cpp tools/rrgame/vr_nunchaku.cpp)
# VR comfort: smooth motion between the race's steps, the view off the bike, the motion meter.
target_sources(rrgame PRIVATE tools/rrgame/vr_comfort.cpp)
# The player's own bike in the head view: the shake meter and the drawn part slots.
target_sources(rrgame PRIVATE tools/rrgame/vr_bike_shake.cpp)
# The VR race stepped on the display's clock: xrWaitFrame's predictedDisplayTime, the mock's timing.
target_sources(rrgame PRIVATE tools/rrgame/vr_pacing.cpp)
# The head view's horizon: the bike's pitch with the road, not its nods; the riders near the eye metered.
target_sources(rrgame PRIVATE tools/rrgame/vr_horizon.cpp)
target_sources(rrrender PRIVATE src/render/render_target.cpp src/render/text_overlay.cpp src/render/smooth_texture.cpp
                                src/render/race_scene_pc.cpp src/render/coplanar.cpp # the coplanar layers
                                src/render/multiview.cpp) # single-pass stereo (GL_OVR_multiview2)

# VR: the OpenXR layer (GL / GLES), the Quest build and the proof target rrvrtest
include(cmake/vr.cmake)
