#ifndef EMULATOR_SRC_UWP_OVERLAY_H_
#define EMULATOR_SRC_UWP_OVERLAY_H_

#include <winrt/Windows.UI.Xaml.Controls.h>

#include <cstdint>
#include <memory>

namespace Kyty::Uwp {

// The overlay over a running game, in the system's look: the game and how long it has run, and
// performance at the top left (frame rate, frame times, memory, shaders), the controllers at the
// top right and the emulator's log at the bottom, at the size the settings choose. Input goes past
// it to the game.
class GameOverlay {
public:
	// UI thread: adds the overlay to the game's page, at `index` among its children.
	GameOverlay(winrt::Windows::UI::Xaml::Controls::Grid const& page, uint32_t index,
	            winrt::hstring const& game_name);
	~GameOverlay();

	void               Show(bool shown);
	[[nodiscard]] bool Shown() const;

private:
	struct State;
	std::shared_ptr<State> m_state;
};

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_OVERLAY_H_
