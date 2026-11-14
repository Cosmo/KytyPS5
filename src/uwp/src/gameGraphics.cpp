#include "gameGraphics.h"

#include "common/emulatorConfig.h"
#include "fileAccess.h"
#include "settingTiles.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace Kyty::Uwp {

namespace {

// The settings page's sizes for "Textures count as big from", and the draw counts the game menu offers.
constexpr std::array<uint32_t, 5> TEXTURE_SIZES = {256, 512, 1024, 2048, 4096};
constexpr std::array<uint32_t, 6> DRAW_COUNTS   = {0, 25, 50, 100, 250, 500};
// A texture keeps the size it was made with: a change reaches the textures the game loads afterwards.
constexpr wchar_t TEXTURES_LATER[] = L"Applies to textures loaded from now on, for example after the next level loads.";

void ApplyTextureSizes(const AppSettings& settings) {
	Config::SetTextureMipSkip(settings.texture_mip_skip, settings.texture_mip_skip_min_size, settings.texture_mip_skip_uncompressed);
}

void ApplyTextureFiltering(const AppSettings& settings) {
	Config::SetTextureFiltering(settings.texture_lod_bias, settings.anisotropic_filtering);
}

} // namespace

void AddGameGraphicsSettings(winrt::Windows::UI::Xaml::Controls::Panel const& tiles, winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation,
                             std::shared_ptr<AppSettings> settings, std::function<void(bool)> output_changed) {
	const auto save = [settings] { SaveSettings(*settings); };

	AddHeading(tiles, L"Picture");
	{
		const std::vector<std::string> values {"Off", "Low", "Medium", "High"};
		const auto                     found = std::find(values.begin(), values.end(), settings->fsr1_upscaling);
		AddChoice(
		    tiles, explanation, L"FSR 1", {L"Off", L"Sharpness low", L"Sharpness medium", L"Sharpness high"},
		    found != values.end() ? static_cast<size_t>(found - values.begin()) : 0,
		    [settings, save, values](size_t index) {
			    const size_t mode        = index < values.size() ? index : 0;
			    settings->fsr1_upscaling = values[mode];
			    Config::SetFsr1Upscaling(static_cast<uint32_t>(mode));
			    save();
		    },
		    {L"Sharpens and upscales games that render below the output resolution. This is experimental and doesn't work with HDR."});
	}
	if (IsXbox()) {
		AddChoice(
		    tiles, explanation, L"Output resolution", {L"Match TV", L"1080p"}, settings->output_resolution == "1080p" ? 1 : 0,
		    [settings, save, output_changed](size_t index) {
			    settings->output_resolution = index == 1 ? "1080p" : "Match TV";
			    save();
			    if (output_changed) {
				    output_changed(index != 1);
			    }
		    },
		    {L"Match TV keeps 4K games sharp on a 4K TV. 1080p uses less memory, and your console scales the picture up to your TV."});
	}
	AddSwitch(
	    tiles, explanation, L"V-Sync", settings->vsync,
	    [settings, save](bool on) {
		    settings->vsync = on;
		    Config::SetPresentMode(on ? Config::PresentMode::Fifo : Config::PresentMode::Mailbox);
		    save();
	    },
	    IsXbox() ? Paragraphs {L"Syncs each frame to your TV's refresh for smooth motion. Turn it off to let games run above 60 frames per second."}
	             : Paragraphs {L"Syncs each frame to the display's refresh for smooth, even motion, at the cost of slightly higher input latency.",
	                           L"When off, frames are shown as soon as they're ready, with lower latency."});

	AddHeading(tiles, L"Frame rate");
#ifdef KYTY_HAS_FSR
	if (Config::FsrFrameGenerationAvailable()) {
		AddSwitch(
		    tiles, explanation, L"FSR frame generation", Config::FsrFrameGenerationEnabled(),
		    [settings, save](bool on) {
			    settings->fsr_frame_generation = on;
			    Config::SetFsrFrameGeneration(on);
			    save();
		    },
		    {L"Adds a generated frame between game frames for smoother motion. Works only with PS5 Pro games that use PSSR and run at half your refresh "
		     L"rate, like 30 fps at 60 Hz. This is experimental."});
	} else {
		// Its resources are made when a game starts: during a game it can only be turned off.
		AddAction(tiles, explanation, L"FSR frame generation", L"Off", [] {},
		          {L"Adds a generated frame between game frames for smoother motion.",
		           L"To use it, turn it on in Settings \u203A Display before you start a game. During a game it can only be turned off."});
	}
#endif
	{
		std::vector<uint32_t> counts(DRAW_COUNTS.begin(), DRAW_COUNTS.end());
		if (std::find(counts.begin(), counts.end(), settings->flush_draws) == counts.end()) {
			counts.push_back(settings->flush_draws);
		}
		std::vector<std::wstring> names;
		size_t                    selected = 0;
		for (size_t index = 0; index < counts.size(); index++) {
			names.push_back(counts[index] == 0 ? std::wstring(L"Off") : std::to_wstring(counts[index]) + L" draws");
			if (counts[index] == settings->flush_draws) {
				selected = index;
			}
		}
		AddChoice(
		    tiles, explanation, L"Draws per GPU submission", std::move(names), selected,
		    [settings, save, counts](size_t index) {
			    settings->flush_draws = counts[index];
			    Config::SetFlushDraws(counts[index]);
			    save();
		    },
		    {L"Submits recorded work after this many draws so the GPU can start before a long command buffer ends.",
		     L"Extra submissions cost CPU time. Off is normal batching."});
	}

	AddHeading(tiles, L"Textures");
	AddChoice(
	    tiles, explanation, L"Texture detail", {L"Full", L"Half resolution", L"Quarter resolution", L"Eighth resolution", L"Sixteenth resolution"},
	    std::min<uint32_t>(settings->texture_mip_skip, 4),
	    [settings, save](size_t index) {
		    settings->texture_mip_skip = static_cast<uint32_t>(index);
		    ApplyTextureSizes(*settings);
		    save();
	    },
	    {L"Big compressed textures with mipmaps load at a lower resolution to save GPU memory.", TEXTURES_LATER});
	{
		std::vector<std::wstring> names;
		size_t                    selected = 3;
		for (size_t index = 0; index < TEXTURE_SIZES.size(); index++) {
			names.push_back(std::to_wstring(TEXTURE_SIZES[index]) + L" px");
			if (TEXTURE_SIZES[index] == settings->texture_mip_skip_min_size) {
				selected = index;
			}
		}
		AddChoice(
		    tiles, explanation, L"Textures count as big from", std::move(names), selected,
		    [settings, save](size_t index) {
			    settings->texture_mip_skip_min_size = TEXTURE_SIZES[index];
			    ApplyTextureSizes(*settings);
			    save();
		    },
		    {L"Only textures at least this large get a lower resolution. The size is the longer side, in pixels.", TEXTURES_LATER});
	}
	AddSwitch(
	    tiles, explanation, L"Shrink uncompressed textures too", settings->texture_mip_skip_uncompressed,
	    [settings, save](bool on) {
		    settings->texture_mip_skip_uncompressed = on;
		    ApplyTextureSizes(*settings);
		    save();
	    },
	    {L"Uncompressed textures with mipmaps get a lower resolution as well.", TEXTURES_LATER});
	AddChoice(
	    tiles, explanation, L"Texture sharpness", {L"Full", L"One level softer", L"Two levels softer", L"Three levels softer", L"Four levels softer"},
	    std::min<uint32_t>(settings->texture_lod_bias, 4),
	    [settings, save](size_t index) {
		    settings->texture_lod_bias = static_cast<uint32_t>(index);
		    ApplyTextureFiltering(*settings);
		    save();
	    },
	    {L"Textures are drawn from their smaller versions. This saves no memory.", L"Can be a little faster when the GPU is the limit, but blurrier."});
	AddSwitch(
	    tiles, explanation, L"Anisotropic filtering", settings->anisotropic_filtering,
	    [settings, save](bool on) {
		    settings->anisotropic_filtering = on;
		    ApplyTextureFiltering(*settings);
		    save();
	    },
	    {L"Keeps textures sharp on surfaces seen at an angle, where the game asks for it.", L"Off uses plain filtering instead. It can be a little faster."});
}

} // namespace Kyty::Uwp
