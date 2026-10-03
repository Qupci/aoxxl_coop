// Renderer / RwCamera access for split-screen.
//
// FUN_004144A0 (the renderer init) stashes its objects in globals:
//   0x662EB4  frame-buffer object
//   0x662EB8  the main 3D RwCamera
//   0x662EBC  the 2D/HUD RwCamera (ortho, view window 0.5/0.5, near 0.1 far 1.9)
//
// The camera is stock RenderWare 3. Confirmed against the engine's own calls:
// FUN_0058FFB0 writes viewWindow at +0x68/+0x6C and recipViewWindow at
// +0x70/+0x74 then syncs the frame at +0x04, and the init calls
// SetNearClipPlane(0.1) / SetFarClipPlane(100) / SetProjection(2=perspective).

#pragma once
#include "kal.h"

struct RwCamera {
	uint8_t  object[0x14];      // RwObjectHasFrame; parent frame at +0x04
	uint32_t projectionType;    // +0x14
	void*    beginUpdate;       // +0x18
	void*    endUpdate;         // +0x1C
	float    viewMatrix[16];    // +0x20
	void*    frameBuffer;       // +0x60  RwRaster*
	void*    zBuffer;           // +0x64  RwRaster*
	float    viewWindow[2];     // +0x68
	float    recipViewWindow[2];// +0x70
	float    viewOffset[2];     // +0x78
	float    nearPlane;         // +0x80
	float    farPlane;          // +0x84
	float    fogPlane;          // +0x88
};

namespace axr {
const uintptr_t kFrameBufferObj = 0x662EB4;
const uintptr_t kMainCameraPtr  = 0x662EB8;
const uintptr_t kHudCameraPtr   = 0x662EBC;

// RenderWare 3 entry points identified from the renderer init.
const uintptr_t kRwCameraCreate         = 0x590060;
const uintptr_t kRwCameraSetViewWindow  = 0x58FFB0;
const uintptr_t kRwCameraSetViewOffset  = 0x58FD40;
const uintptr_t kRwCameraSetNearClip    = 0x58FD70;
const uintptr_t kRwCameraSetFarClip     = 0x58FE40;
const uintptr_t kRwCameraSetProjection  = 0x58FF40;
const uintptr_t kCalcProjCamera         = 0x414B70;

// FUN_00413FF0 -- "render one view". __thiscall with one stack argument
// (ends in RET 4). Between BeginUpdate and EndUpdate it calls 0x414110, the
// scene draw. Running this twice, with the camera moved in between, is
// split-screen.
const uintptr_t kRenderView             = 0x413FF0;
const uintptr_t kSceneDraw              = 0x414110;
const uintptr_t kRwCameraBeginUpdate    = 0x58FD30;
const uintptr_t kRwCameraEndUpdate      = 0x58FD20;
}

inline RwCamera* MainCamera() { return *(RwCamera**)axr::kMainCameraPtr; }
inline RwCamera* HudCamera()  { return *(RwCamera**)axr::kHudCameraPtr; }

// Installs caller-capture thunks over the main camera's beginUpdate/endUpdate
// pointers. Whoever calls beginUpdate is the scene render, which is the thing
// split-screen has to run twice.
void RenderProbeTick();

// Installs the split-screen render hook. Safe to call once at startup.
bool RenderInstallHooks();

extern volatile long g_renderPasses;   // scene renders issued, for the watchdog
