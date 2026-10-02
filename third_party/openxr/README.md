The Khronos OpenXR API headers (openxr.h, openxr_platform.h, ... version 1.1.58), copied unchanged from the
gt2-play project's third_party/openxr (itself a copy of the OpenXR-SDK release headers).

Every header carries `SPDX-License-Identifier: Apache-2.0 OR MIT`; this project uses them under the MIT option
(only MIT/CC0/ISC code is reused in this project). `LICENSE` is the SDK's license file as shipped with them.

Headers only. The loader itself is never committed: Windows loads `openxr_loader.dll` from next to the executable
at run time (CMake copies one that already exists on this machine, see cmake/vr.cmake), and the Quest APK packages
the loader of the Khronos `openxr_loader_for_android` AAR found on disk (scripts/build-quest.ps1).
