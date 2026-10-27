#include "controllerSettings.h"

#include "dsu.h"
#include "input.h"
#include "xboxLook.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

// The tiles follow the controllers and servers this often.
constexpr auto REFRESH_INTERVAL = std::chrono::milliseconds(250);
constexpr int  PLAYERS          = 4;

// Tiles that follow the controllers and servers: made anew (the focus kept on the same tile) when what they show changes, checked every
// REFRESH_INTERVAL while they are on the page; not while a choice, a menu or a box is open.
struct LiveTiles {
	std::vector<xaml::Controls::StackPanel> panels;
	std::function<std::wstring()>           shown;
	std::function<void()>                   build;
	std::wstring                            last;
	xaml::DispatcherTimer                   timer {nullptr};
};

void Rebuild(LiveTiles& live) {
	live.last = live.shown();
	for (const auto& panel: live.panels) {
		panel.Children().Clear();
	}
	live.build();
}

void Refresh(LiveTiles& live) {
	if (live.shown() == live.last || xaml::Media::VisualTreeHelper::GetOpenPopups(xaml::Window::Current()).Size() != 0) {
		return;
	}
	winrt::hstring tag;
	if (const auto focused = xaml::Input::FocusManager::GetFocusedElement().try_as<xaml::FrameworkElement>()) {
		for (const auto& panel: live.panels) {
			uint32_t index = 0;
			if (panel.Children().IndexOf(focused, index)) {
				tag = winrt::unbox_value_or<winrt::hstring>(focused.Tag(), L"");
			}
		}
	}
	Rebuild(live);
	if (!tag.empty()) {
		for (const auto& panel: live.panels) {
			if (FocusFirst(panel, tag)) {
				break;
			}
		}
	}
}

// Puts each of `panels` in its place (one inner panel each, so that a place may hold other things too), keeps `owner` while the first is on
// the page and follows meanwhile.
template <typename Owner>
void Follow(const std::shared_ptr<Owner>& owner, LiveTiles& live, std::initializer_list<xaml::Controls::Panel> places) {
	for (const auto& place: places) {
		xaml::Controls::StackPanel panel;
		panel.Spacing(8);
		place.Children().Append(panel);
		live.panels.push_back(panel);
	}
	Rebuild(live);
	struct Keeper {
		std::shared_ptr<Owner> owner;
	};
	auto                       keeper = std::make_shared<Keeper>(Keeper {owner});
	const std::weak_ptr<Owner> weak   = owner;
	const auto                 first  = live.panels.front();
	first.Loaded([weak](auto const&, auto const&) {
		auto owner = weak.lock();
		if (owner == nullptr) {
			return;
		}
		auto& live = owner->live;
		if (live.timer == nullptr) {
			live.timer = xaml::DispatcherTimer();
			live.timer.Interval(REFRESH_INTERVAL);
			live.timer.Tick([weak](auto const&, auto const&) {
				if (auto owner = weak.lock()) {
					Refresh(owner->live);
				}
			});
		}
		live.timer.Start();
	});
	first.Unloaded([keeper](auto const&, auto const&) {
		if (keeper->owner != nullptr && keeper->owner->live.timer != nullptr) {
			keeper->owner->live.timer.Stop();
		}
		keeper->owner.reset();
	});
}

// Saves the settings and applies them.
void Apply(const std::shared_ptr<AppSettings>& settings, const std::function<void()>& save) {
	save();
	SetControllerAssignment(settings->controller_auto, settings->controller_players, DsuServersInUse(*settings));
}

std::wstring PlayerName(int player) {
	return player < 0 ? std::wstring(L"Not playing") : L"Player " + std::to_wstring(player + 1);
}

std::vector<std::wstring> Lines(const std::wstring& text) {
	std::vector<std::wstring> lines;
	size_t                    start = 0;
	while (start <= text.size()) {
		const size_t end = std::min(text.find(L'\n', start), text.size());
		if (end > start) {
			lines.push_back(text.substr(start, end - start));
		}
		start = end + 1;
	}
	return lines;
}

std::wstring Trimmed(std::wstring text) {
	text.erase(0, text.find_first_not_of(L' '));
	text.erase(text.find_last_not_of(L' ') + 1);
	return text;
}

// ---- Players ----

struct PlayersPage {
	std::shared_ptr<AppSettings> settings;
	std::function<void()>        save;
	std::function<void()>        open_servers;
	xaml::Controls::StackPanel   explanation {nullptr};
	xaml::Controls::Border       hints {nullptr};
	LiveTiles                    live;
};

std::wstring PlayersShown(const PlayersPage& page) {
	std::wstring shown = page.settings->controller_auto ? L"auto\n" : L"manual\n";
	for (const auto& controller: ConnectedControllers()) {
		shown += std::wstring(winrt::to_hstring(controller.id)) + L"=" + std::to_wstring(controller.player) + L" " + controller.name + L" " +
		         controller.detail + (controller.latest ? L" *" : L"") + (controller.can_identify ? L" y" : L"") +
		         (controller.light_bar ? L" l\n" : L"\n");
	}
	return shown;
}

// Manual from now on, with each connected controller where it plays now.
void SwitchToManual(AppSettings& settings) {
	settings.controller_auto = false;
	for (const auto& controller: ConnectedControllers()) {
		settings.controller_players[controller.id] = controller.player;
	}
}

// The hint bar while a controller's tile has the focus (with Y to identify it), and otherwise.
void ShowHints(const PlayersPage& page, bool controller) {
	if (page.hints == nullptr) {
		return;
	}
	if (controller) {
		SetHints(page.hints, {{XboxButton::A, L"Select"}, {XboxButton::Y, L"Identify"}, {XboxButton::B, L"Back"}});
	} else {
		SetHints(page.hints, {{XboxButton::A, L"Select"}, {XboxButton::B, L"Back"}});
	}
}

// A controller's tile: its name and its player; its text tells its connection and battery (and a DSU controller's server), A chooses
// its player, Y makes it vibrate.
void AddControllerTile(const std::shared_ptr<PlayersPage>& page, const ControllerInfo& controller) {
	const std::weak_ptr<PlayersPage> weak  = page;
	const auto&                      panel = page->live.panels.back();
	std::vector<std::wstring>        choices;
	for (int player = -1; player < PLAYERS; player++) {
		choices.push_back(PlayerName(player));
	}
	auto tile = AddChoice(panel, page->explanation, controller.name.c_str(), std::move(choices), static_cast<size_t>(controller.player + 1),
	                      [weak, id = controller.id](size_t index) {
		                      if (auto page = weak.lock()) {
			                      if (page->settings->controller_auto) {
				                      SwitchToManual(*page->settings);
			                      }
			                      page->settings->controller_players[id] = static_cast<int>(index) - 1;
			                      Apply(page->settings, page->save);
		                      }
	                      });
	tile.Tag(winrt::box_value(winrt::to_hstring("c:" + controller.id)));

	// The facts: the player, the connection and battery, which one was used last; a DSU controller's server on a line of its own.
	auto facts = Lines(controller.detail);
	if (facts.empty()) {
		facts.emplace_back();
	}
	facts[0] = PlayerName(controller.player) + (facts[0].empty() ? L"" : L" · " + facts[0]) + (controller.latest ? L" · Last used" : L"");
	const std::wstring identify =
	    !controller.can_identify ? L"This controller can't vibrate, because its server doesn't support it."
	    : controller.light_bar   ? L"Press [Y] to make it vibrate and light up white, so you can tell which controller it is."
	                             : L"Press [Y] to make it vibrate, so you can tell which controller it is.";
	Explain(tile, page->explanation, controller.name, facts, {L"Select it to choose its player.", identify});
	// While the list of players is open.
	tile.Click([weak, name = controller.name, facts](auto const&, auto const&) {
		if (auto page = weak.lock()) {
			Paragraphs text = {L"Not playing: the controller isn't used in games."};
			if (page->settings->controller_auto) {
				text.emplace_back(L"Choosing a player switches Assign controllers to Manually.");
			}
			ShowExplanation(page->explanation, name, facts, text);
		}
	});
	tile.KeyDown([id = controller.id, can = controller.can_identify](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		if (args.Key() == winrt::Windows::System::VirtualKey::GamepadY || args.Key() == winrt::Windows::System::VirtualKey::I) {
			args.Handled(true);
			if (can) {
				IdentifyController(id);
			}
		}
	});
	tile.GotFocus([weak](auto const&, auto const&) {
		if (auto page = weak.lock()) {
			ShowHints(*page, true);
		}
	});
	tile.LostFocus([weak](auto const&, auto const&) {
		if (auto page = weak.lock()) {
			ShowHints(*page, false);
		}
	});
}

void BuildPlayers(const std::shared_ptr<PlayersPage>& page) {
	const std::weak_ptr<PlayersPage> weak        = page;
	const auto&                      assign      = page->live.panels.front();
	const auto&                      list        = page->live.panels.back();
	const auto&                      explanation = page->explanation;

	AddHeading(assign, L"Assign");
	auto mode = AddChoice(
	    assign, explanation, L"Assign controllers", {L"Automatically", L"Manually"}, page->settings->controller_auto ? 0 : 1,
	    [weak](size_t index) {
		    if (auto page = weak.lock()) {
			    if (index == 0) {
				    page->settings->controller_auto = true;
			    } else {
				    SwitchToManual(*page->settings);
			    }
			    Apply(page->settings, page->save);
		    }
	    },
	    {L"Automatically: each controller is assigned to the next free player when it connects.",
	     page->settings->dsu_enabled ? L"Manually: you choose the player for each controller. Several controllers can share a player, for example "
	                                   L"a phone for motion controls together with an Xbox controller."
	                                 : L"Manually: you choose the player for each controller. Several controllers can share a player.",
	     L"Any Xbox controller can open the game menu with [View] + [Menu], whichever player it's assigned to."});
	mode.Tag(winrt::box_value(L"mode"));
	// Only with Settings › Developer › Remote Controllers on.
	if (page->settings->dsu_enabled) {
		auto servers = AddAction(
		    assign, explanation, L"Remote Controllers", L"",
		    [weak] {
			    if (auto page = weak.lock()) {
				    page->open_servers();
			    }
		    },
		    {L"Add Remote Controllers to use controllers that DSU-compatible apps share over your network."});
		servers.Tag(winrt::box_value(L"servers"));
	}

	AddHeading(list, L"Controllers");
	const auto controllers = ConnectedControllers();
	if (controllers.empty()) {
		auto none = AddAction(list, explanation, L"No controllers connected", L"", [] {});
		none.IsEnabled(false);
	}
	for (const auto& controller: controllers) {
		AddControllerTile(page, controller);
	}
}

// ---- DSU servers ----

// What DSU servers are, and how to add one.
Paragraphs DsuServersText() {
	return {L"A DSU-compatible app on your PC or phone shares its controllers over your network.",
	        L"Select Add server and enter the address the app shows."};
}

struct ServersPage {
	std::shared_ptr<AppSettings> settings;
	std::function<void()>        save;
	xaml::Controls::StackPanel   about {nullptr};
	LiveTiles                    live;
};

// What a server answers, for its tile.
std::wstring ServerStatus(const std::string& server) {
	const int controllers = Dsu::ServerControllers(server);
	if (controllers > 0) {
		return controllers == 1 ? std::wstring(L"1 controller") : std::to_wstring(controllers) + L" controllers";
	}
	return controllers < 0 ? L"No controllers" : L"Not responding";
}

std::wstring ServersShown(const ServersPage& page) {
	std::wstring shown;
	for (const auto& server: page.settings->dsu_servers) {
		shown += std::wstring(winrt::to_hstring(server)) + L":" + ServerStatus(server) + L"\n";
	}
	return shown;
}

// Asks for a server's address; Add (or Enter, or the on-screen keyboard's Go) adds it.
winrt::fire_and_forget AskForServer(std::weak_ptr<ServersPage> weak) {
	xaml::Controls::TextBox address;
	address.Header(winrt::box_value(L"Server address"));
	address.PlaceholderText(L"192.168.1.20");
	{
		xaml::Input::InputScopeName name;
		name.NameValue(xaml::Input::InputScopeNameValue::Url);
		xaml::Input::InputScope scope;
		scope.Names().Append(name);
		address.InputScope(scope);
	}
	xaml::Controls::ContentDialog dialog;
	dialog.Title(winrt::box_value(L"Add a server"));
	dialog.Content(address);
	dialog.PrimaryButtonText(L"Add");
	dialog.CloseButtonText(L"Cancel");
	dialog.DefaultButton(xaml::Controls::ContentDialogButton::Primary);
	auto entered = std::make_shared<bool>(false);
	address.KeyDown([dialog = winrt::make_weak(dialog), entered](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
			args.Handled(true);
			*entered = true;
			if (auto shown = dialog.get()) {
				shown.Hide();
			}
		}
	});
	dialog.Opened([address](auto const&, auto const&) { address.Focus(xaml::FocusState::Programmatic); });
	const auto result = co_await dialog.ShowAsync();
	if (result != xaml::Controls::ContentDialogResult::Primary && !*entered) {
		co_return;
	}
	auto page = weak.lock();
	if (page == nullptr) {
		co_return;
	}
	const auto text    = winrt::to_string(Trimmed(std::wstring(address.Text())));
	auto&      servers = page->settings->dsu_servers;
	if (!text.empty() && std::find(servers.begin(), servers.end(), text) == servers.end()) {
		servers.push_back(text);
		Apply(page->settings, page->save);
	}
	Rebuild(page->live);
	page->live.panels.front().UpdateLayout();
	FocusFirst(page->live.panels.front(), text.empty() ? winrt::hstring(L"add") : winrt::to_hstring("s:" + text));
}

void BuildServers(const std::shared_ptr<ServersPage>& page) {
	const std::weak_ptr<ServersPage> weak  = page;
	const auto&                      panel = page->live.panels.front();
	const auto                       text  = DsuServersText();

	AddHeading(panel, L"Servers");
	for (const auto& server: page->settings->dsu_servers) {
		const auto status = ServerStatus(server);
		auto       tile   = AddAction(panel, page->about, winrt::to_hstring(server).c_str(), status.c_str(), [] {}, text);
		tile.Tag(winrt::box_value(winrt::to_hstring("s:" + server)));
		tile.Click([weak, server, tile = winrt::make_weak(tile)](auto const&, auto const&) {
			auto shown = tile.get();
			if (shown == nullptr) {
				return;
			}
			xaml::Controls::MenuFlyout     menu;
			xaml::Controls::MenuFlyoutItem remove;
			remove.Text(L"Remove server");
			remove.Click([weak, server](auto const&, auto const&) {
				if (auto page = weak.lock()) {
					auto& servers = page->settings->dsu_servers;
					servers.erase(std::remove(servers.begin(), servers.end(), server), servers.end());
					Apply(page->settings, page->save);
					Rebuild(page->live);
					page->live.panels.front().UpdateLayout();
					FocusFirst(page->live.panels.front(), L"add");
				}
			});
			xaml::Controls::MenuFlyoutItem cancel;
			cancel.Text(L"Cancel");
			menu.Items().Append(remove);
			menu.Items().Append(cancel);
			menu.ShowAt(shown);
		});
	}
	auto add = AddAction(
	    panel, page->about, L"Add server", L"",
	    [weak] {
		    if (!weak.expired()) {
			    AskForServer(weak);
		    }
	    },
	    text);
	add.Tag(winrt::box_value(L"add"));
}

void AddPlayersSettings(xaml::Controls::Panel const& assign, xaml::Controls::Panel const& controllers, xaml::Controls::StackPanel const& explanation,
                        xaml::Controls::Border const& hints, std::shared_ptr<AppSettings> settings, std::function<void()> save,
                        std::function<void()> open_servers) {
	auto page          = std::make_shared<PlayersPage>();
	page->settings     = std::move(settings);
	page->save         = std::move(save);
	page->open_servers = std::move(open_servers);
	page->explanation  = explanation;
	page->hints        = hints;
	const std::weak_ptr<PlayersPage> weak = page;
	page->live.shown = [weak] {
		auto page = weak.lock();
		return page != nullptr ? PlayersShown(*page) : std::wstring();
	};
	page->live.build = [weak] {
		if (auto page = weak.lock()) {
			BuildPlayers(page);
		}
	};
	Follow(page, page->live, {assign, controllers});
}

void AddServerSettings(xaml::Controls::Panel const& panel, xaml::Controls::StackPanel const& about, std::shared_ptr<AppSettings> settings,
                       std::function<void()> save) {
	auto page      = std::make_shared<ServersPage>();
	page->settings = std::move(settings);
	page->save     = std::move(save);
	page->about    = about;
	ShowExplanation(about, DsuServersText());
	const std::weak_ptr<ServersPage> weak = page;
	page->live.shown = [weak] {
		auto page = weak.lock();
		return page != nullptr ? ServersShown(*page) : std::wstring();
	};
	page->live.build = [weak] {
		if (auto page = weak.lock()) {
			BuildServers(page);
		}
	};
	Follow(page, page->live, {panel});
}

} // namespace

void AddPlayersColumns(xaml::Controls::StackPanel const& columns, xaml::Controls::StackPanel const& explanation, xaml::Controls::Border const& hints,
                       std::shared_ptr<AppSettings> settings, std::function<void()> save, std::function<void()> open_servers) {
	const auto assign      = AddColumn(columns);
	const auto controllers = AddColumn(columns);
	AddPlayersSettings(assign, controllers, explanation, hints, std::move(settings), std::move(save), std::move(open_servers));
}

void AddPlayersTiles(xaml::Controls::Panel const& panel, xaml::Controls::StackPanel const& explanation, std::shared_ptr<AppSettings> settings,
                     std::function<void()> save, std::function<void()> open_servers) {
	AddPlayersSettings(panel, panel, explanation, nullptr, std::move(settings), std::move(save), std::move(open_servers));
}

void AddServerTiles(xaml::Controls::Panel const& panel, xaml::Controls::StackPanel const& about, std::shared_ptr<AppSettings> settings,
                    std::function<void()> save) {
	AddServerSettings(panel, about, std::move(settings), std::move(save));
}

void AddServerColumns(xaml::Controls::StackPanel const& columns, std::shared_ptr<AppSettings> settings, std::function<void()> save) {
	const auto servers = AddColumn(columns);
	// What DSU servers are, in a box as the Xbox explains one setting of a page.
	const auto                 about = AddColumn(columns, L"How it works");
	xaml::Controls::Border     box;
	xaml::Controls::StackPanel text;
	box.Background(xaml::Media::SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x2B, 0x2B, 0x2B)));
	box.Padding(xaml::ThicknessHelper::FromLengths(11, 12, 11, 12));
	box.MinHeight(247);
	text.Spacing(12);
	box.Child(text);
	about.Children().Append(box);
	AddServerSettings(servers, text, std::move(settings), std::move(save));
}

} // namespace Kyty::Uwp
