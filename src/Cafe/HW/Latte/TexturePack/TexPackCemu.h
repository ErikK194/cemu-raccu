#pragma once
// Cemu glue for the texture-pack texture key, the texture-pack original-texture dumper and the texture-pack KTX2 replacement.
// Everything Cemu specific (LatteTexture, guest memory, settings, paths, renderer) lives here and in the few hooks
// that call it; the texture-pack logic itself is in TexPackKey / TexPackKtx2Loader / TexPackKtx2Pack (emulator neutral).

#include "Cafe/HW/Latte/TexturePack/TexPackKtx2Loader.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"

#include <optional>

class LatteTexture;

namespace TexPackCemu
{
	// ---------------- key + dump ----------------

	// true while any texture-pack feature needs keys (dump or replacement)
	bool IsKeyingActive();
	bool IsDumpActive();

	// Compute the texture-pack key of a texture's level 0 from the bytes currently in guest memory.
	// The key is computed only when the surface matches the verified TEX profile exactly
	// (format, single 2D image, profile tile mode, profile span length).
	texpack::KeyStatus ComputeKey(const LatteTexture* tex, MPTR physImagePtr, texpack::TextureKey& keyOut);

	// Record the key (or the reason for a missing key) in dump/texture_pack/<titleId>/keys.csv (once per distinct entry).
	void LogKey(const LatteTexture* tex, MPTR physImagePtr, texpack::KeyStatus status, const texpack::TextureKey& key);

	// Queue one decoded original mip level (tightly packed RGBA8, width*height*4 bytes) for PNG export.
	// Each {key, mip} combination is written at most once per session and never overwrites an existing file
	// (docs/texpack/SPEC.md section 10).
	void QueueDump(const texpack::TextureKey& key, uint32 mipIndex, uint32 width, uint32 height, std::vector<uint8>&& rgba);

	// Called when a title shuts down (CafeSystem::ShutdownTitle): waits up to 30 s until queued PNGs are written.
	void FlushDumps();

	// ---------------- KTX2 replacement ----------------

	// Set by a renderer that implements texture_loadTexPackReplacement (after checking device format support);
	// std::nullopt = this renderer cannot show replacements. Replacement is active only with a registered target and
	// the user option enabled.
	void SetRendererTargetFormat(std::optional<texpack::TargetFormat> target);
	// largest 2D image edge the device supports; larger replacements start at a smaller file level (never below the
	// original size), the RGBA8 fallback is capped further to bound memory use
	void SetRendererMaxImageEdge(uint32 maxEdge);
	bool IsReplacementActive();

	// Marks textures created inside its scope as eligible for replacement (texture units = sampling).
	// Render targets, copy destinations and readback copies are created outside of it and never replaced.
	class ScopedReplacementEligible
	{
	public:
		explicit ScopedReplacementEligible(bool eligible = true);
		~ScopedReplacementEligible();
	private:
		bool m_prev;
	};

	// LatteTexture constructor hook: computes the key from guest memory and, if the pack has a valid KTX2 for it,
	// sets tex->texPackReplacement and the resolution overwrite. Textures with a graphic-pack size/format rule keep the
	// rule unless the texture-pack priority setting is on.
	void OnTextureCreated(LatteTexture* tex);

	// Uploads all replacement levels of a replaced texture (cache or fresh transcode). false = the replacement is
	// gone or no longer matches the texture object; the caller flags the texture for recreation.
	bool UploadReplacement(LatteTexture* tex);

	// Level-0 load hook: true when the replacement state of `tex` no longer matches the guest data
	// (key changed, replacement appeared or disappeared). keyValid/key describe the freshly computed key.
	bool NeedsRecreate(const LatteTexture* tex, bool keyValid, const texpack::TextureKey& key);
	void FlagRecreate(LatteTexture* tex);
	// GPU thread, between commands:
	// - applies a switch of the user option to existing textures (on: textures with a replacement, off: replaced
	//   textures are flagged for recreation; off also frees the transcode cache), with log + overlay notification
	// - flags textures whose replacement a background worker has finished transcoding (created with the original)
	// - logs a statistics line at most every 30 s when something changed
	void OnGpuCommandBoundary();
	// a replaced texture was deleted because the game renders (renderTarget) or GPU-copies into its memory
	void OnReplacementDropped(bool renderTarget);
	// true (once) when textures were flagged for recreation; LatteTexture_TexPackRecreatePending then rebuilds them and
	// calls RearmPendingRecreates for those left over by the swap budget
	bool TakePendingRecreates();
	void RearmPendingRecreates();
	// per-frame time budget for swapping textures to their replacement (half a 60 Hz frame, PPSSPP's default idea):
	// at least one swap per frame window, further swaps while the window's spent time is below the budget
	bool SwapBudgetAvailable();
	void ChargeSwapTime(double seconds);
}
