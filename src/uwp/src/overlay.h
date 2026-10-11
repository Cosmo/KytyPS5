#ifndef EMULATOR_SRC_UWP_OVERLAY_H_
#define EMULATOR_SRC_UWP_OVERLAY_H_

#include <winrt/Windows.UI.Xaml.Controls.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace Kyty::Uwp {

struct AppSettings;

// The overlay over a running game, in the system's look: the game and how long it has run, and
// performance at the top left (frame rate, frame times, memory, shaders), the controllers at the
// top right and the emulator's log at the bottom, as much of it and at the size the settings
// choose. Input goes past it to the game.
class GameOverlay {
public:
	// UI thread: adds the overlay to the game's page, at `index` among its children.
	GameOverlay(winrt::Windows::UI::Xaml::Controls::Grid const& page, uint32_t index,
	            winrt::hstring const& game_name);
	~GameOverlay();

	void               Show(bool shown);
	[[nodiscard]] bool Shown() const;

	// UI thread: what the overlay shows and its size, from the settings (AppSettings::overlay_*), at once.
	void Apply(const AppSettings& settings);

private:
	struct State;
	std::shared_ptr<State> m_state;
};

// The overlay's settings as tiles, for the settings page and the game menu: what it shows (Off is OverlayShown), its log, its controllers and
// its size, changed in `settings`. `changed` runs after each change.
void AddOverlaySettings(winrt::Windows::UI::Xaml::Controls::Panel const&      panel,
                        winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation, std::shared_ptr<AppSettings> settings,
                        std::function<void()> changed);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_OVERLAY_H_
