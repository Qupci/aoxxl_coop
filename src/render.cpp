#include "render.h"
#include "config.h"
#include "detour.h"
#include "log.h"
#include "vtprofile.h"

volatile long g_renderPasses = 0;

namespace {

// PUSH ESI / MOV ESI,ECX / TEST byte ptr [ESI+0x68],0x20 -- seven bytes, which
// is the first instruction boundary at or past five, so the jmp does not split
// anything.
const uint8_t kRenderViewPrologue[7] = { 0x56, 0x8B, 0xF1, 0xF6, 0x46, 0x68, 0x20 };

typedef void(__fastcall* RenderViewFn)(void* self, void* edx, void* arg);
RenderViewFn g_origRenderView = nullptr;

// First milestone: run the scene render twice, unchanged. This proves the draw
// is safely re-entrant within a frame before any camera is touched -- which is
// the single biggest risk in the whole approach. Expect an identical picture at
// roughly half the frame rate; the second pass simply overdraws the first.
void __fastcall RenderViewHook(void* self, void* edx, void* arg) {
	InterlockedIncrement(&g_renderPasses);
	g_origRenderView(self, edx, arg);

	if (!g_config.splitTest) return;
	InterlockedIncrement(&g_renderPasses);
	g_origRenderView(self, edx, arg);
}

} // namespace

bool RenderInstallHooks() {
	if (!g_config.splitScreen && !g_config.splitTest) return true;
	return InstallDetour(axr::kRenderView, (void*)RenderViewHook,
	                     (void**)&g_origRenderView, kRenderViewPrologue,
	                     sizeof(kRenderViewPrologue), "renderView");
}

void RenderProbeTick() {
	if (!g_config.renderProbe) return;

	static bool installed = false;
	if (installed) return;

	RwCamera* cam = MainCamera();
	if (!cam) return;

	// Sanity-check the layout before trusting it: a perspective camera with a
	// frame buffer and sane clip planes. If this does not hold, the struct model
	// is wrong and hooking function pointers inside it would be reckless.
	// rwPERSPECTIVE is 1 and rwPARALLEL is 2 in RenderWare 3, so the main 3D
	// camera reports 1 here. (The renderer init sets 2 on the *other* camera at
	// +0x14, which is the orthographic one used for the HUD.)
	if (cam->projectionType != 1 || !cam->frameBuffer ||
	    cam->nearPlane <= 0.0f || cam->farPlane <= cam->nearPlane) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			Log("render: camera %p does not match the RW3 layout "
				"(proj=%u fb=%p zb=%p near=%.3f far=%.1f) -- not probing",
				cam, cam->projectionType, cam->frameBuffer, cam->zBuffer,
				cam->nearPlane, cam->farPlane);
		}
		return;
	}

	Log("render: main camera %p  fb=%p zb=%p  viewWindow=(%.4f %.4f) "
		"viewOffset=(%.4f %.4f) near=%.3f far=%.1f",
		cam, cam->frameBuffer, cam->zBuffer,
		cam->viewWindow[0], cam->viewWindow[1],
		cam->viewOffset[0], cam->viewOffset[1],
		cam->nearPlane, cam->farPlane);
	Log("render: hud camera %p", HudCamera());

	// beginUpdate and endUpdate sit next to each other at +0x18, so the existing
	// slot profiler works verbatim -- it just needs a base and a count.
	VtProfileInstall((uintptr_t)cam + 0x18, 2, "RwCamera.begin/endUpdate");
	installed = true;
}
