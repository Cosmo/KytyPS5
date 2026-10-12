#include "keyboard.h"

#include "fileAccess.h"
#include "log.h"
#include "xboxLook.h"
#include "libs/ime.h"
#include "libs/imeDialog.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Kyty::Uwp {

namespace {

namespace xaml      = winrt::Windows::UI::Xaml;
namespace CoreIme   = Libs::Ime;
namespace DialogIme = Libs::Dialog::ImeDialog;
namespace ImeCommon = Libs::ImeCommon;

// At the top, where the Xbox's on-screen keyboard, which comes up from the bottom, leaves it in sight; the game dimmed behind it.
constexpr wchar_t KeyboardMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      Visibility="Collapsed">
  <Rectangle Fill="{ThemeResource SystemControlPageBackgroundMediumAltMediumBrush}" />
  <Grid Width="560" HorizontalAlignment="Center" VerticalAlignment="Top" Margin="0,48,0,0" Padding="24"
        Background="{ThemeResource SystemControlBackgroundChromeMediumLowBrush}">
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto" />
      <RowDefinition Height="Auto" />
      <RowDefinition Height="Auto" />
    </Grid.RowDefinitions>
    <!-- Spaced by margins, not RowSpacing: a game that gives no title leaves no gap for it. -->
    <TextBlock x:Name="KeyboardTitle" Style="{StaticResource SubtitleTextBlockStyle}" MaxLines="2" TextTrimming="CharacterEllipsis"
               Margin="0,0,0,16" />
    <TextBox x:Name="KeyboardText" Grid.Row="1" IsSpellCheckEnabled="False" IsTextPredictionEnabled="False" />
    <Grid Grid.Row="2" Margin="0,16,0,0">
      <Border x:Name="KeyboardHints" Style="{StaticResource HintBarStyle}" HorizontalAlignment="Left" VerticalAlignment="Center" />
      <Button x:Name="KeyboardDone" HorizontalAlignment="Right" MinWidth="120" Style="{StaticResource AccentButtonStyle}" />
    </Grid>
  </Grid>
</Grid>)xaml";

// Which keyboard of the game's: its own text field (sceIme) or the system's keyboard dialog (sceImeDialog), and which time it opened.
enum class Source { None, TextField, Dialog };

struct Session {
	Source   source     = Source::None;
	uint64_t generation = 0;

	bool operator==(const Session&) const = default;
};

struct Keyboard {
	xaml::Controls::Grid      root {nullptr};
	xaml::Controls::TextBlock title {nullptr};
	xaml::Controls::TextBox   text {nullptr};
	xaml::Controls::Button    done {nullptr};
	std::function<void(bool)> shown;
	Session                   session;
};

// UI thread only, but the dispatcher: the visibility callbacks run on the game's threads and post to it; it is set before they are.
Keyboard                                 g_keyboard;
winrt::Windows::UI::Core::CoreDispatcher g_dispatcher {nullptr};

bool ControllerTypes(const ImeCommon::HostSnapshot& snapshot) {
	return (snapshot.disable_device & ImeCommon::DISABLE_DEVICE_CONTROLLER) == 0;
}

// The keyboard the game has open, unless the game reads the controller itself while it is open (DISABLE_DEVICE_CONTROLLER).
std::optional<std::pair<Session, ImeCommon::HostSnapshot>> OpenKeyboard() {
	ImeCommon::HostSnapshot snapshot;
	if (CoreIme::GetHostSnapshot(&snapshot) && ControllerTypes(snapshot)) {
		return std::pair {Session {Source::TextField, snapshot.generation}, snapshot};
	}
	if (DialogIme::GetHostSnapshot(&snapshot) && ControllerTypes(snapshot)) {
		return std::pair {Session {Source::Dialog, snapshot.generation}, snapshot};
	}
	return std::nullopt;
}

winrt::hstring ToHstring(std::u16string_view text) {
	return winrt::hstring(std::wstring_view(reinterpret_cast<const wchar_t*>(text.data()), text.size()));
}

std::u16string ToU16(winrt::hstring const& text) {
	return std::u16string(reinterpret_cast<const char16_t*>(text.c_str()), text.size());
}

// The Done button's label, as the game names it.
winrt::hstring DoneLabel(ImeCommon::EnterLabel label) {
	switch (label) {
		case ImeCommon::EnterLabel::Send: return L"Send";
		case ImeCommon::EnterLabel::Search: return L"Search";
		case ImeCommon::EnterLabel::Go: return L"Go";
		default: return L"Done";
	}
}

// The on-screen keyboard's layout for the kind of text the game asks for.
xaml::Input::InputScope ScopeFor(ImeCommon::Type type) {
	xaml::Input::InputScopeName name;
	switch (type) {
		case ImeCommon::Type::Number: name.NameValue(xaml::Input::InputScopeNameValue::Number); break;
		case ImeCommon::Type::Url: name.NameValue(xaml::Input::InputScopeNameValue::Url); break;
		case ImeCommon::Type::Mail: name.NameValue(xaml::Input::InputScopeNameValue::EmailSmtpAddress); break;
		default: name.NameValue(xaml::Input::InputScopeNameValue::Default); break;
	}
	xaml::Input::InputScope scope;
	scope.Names().Append(name);
	return scope;
}

void Show(const ImeCommon::HostSnapshot& snapshot) {
	auto& keyboard = g_keyboard;
	keyboard.title.Text(ToHstring(snapshot.title));
	keyboard.title.Visibility(snapshot.title.empty() ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
	keyboard.text.MaxLength(static_cast<int32_t>(snapshot.max_text_length));
	keyboard.text.InputScope(ScopeFor(snapshot.type));
	keyboard.text.PlaceholderText(ToHstring(snapshot.placeholder));
	keyboard.text.Text(ToHstring(snapshot.text));
	keyboard.text.SelectionStart(static_cast<int32_t>(keyboard.text.Text().size()));
	keyboard.done.Content(winrt::box_value(DoneLabel(snapshot.enter_label)));
	keyboard.root.Visibility(xaml::Visibility::Visible);
	keyboard.root.UpdateLayout();
	keyboard.text.Focus(xaml::FocusState::Programmatic);
	// On the Xbox, asks for the on-screen keyboard at once, so that the player need not press A on the box first; the log says whether it came.
	const bool keyboard_up = IsXbox() && winrt::Windows::UI::ViewManagement::Core::CoreInputView::GetForCurrentView().TryShow();
	Log("keyboard: the game asks for text (%s, at most %u characters)%s\n", keyboard.session.source == Source::Dialog ? "dialog" : "text field",
	    snapshot.max_text_length, keyboard_up ? ", on-screen keyboard shown" : "");
	if (keyboard.shown) {
		keyboard.shown(true);
	}
}

void Hide() {
	g_keyboard.root.Visibility(xaml::Visibility::Collapsed);
	Log("keyboard: closed\n");
	if (g_keyboard.shown) {
		g_keyboard.shown(false);
	}
}

// Shows the keyboard the game has open now, or hides the box when it has none; the box keeps what the player typed while it is the same one.
void Refresh() {
	const auto open    = OpenKeyboard();
	const auto session = open ? open->first : Session {};
	if (session == g_keyboard.session) {
		return;
	}
	const bool was_shown = g_keyboard.session.source != Source::None;
	g_keyboard.session   = session;
	if (open) {
		Show(open->second);
	} else if (was_shown) {
		Hide();
	}
}

void OnVisibilityChanged(bool /*visible*/, uint64_t /*generation*/) {
	if (g_dispatcher != nullptr) {
		(void)g_dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [] { Refresh(); });
	}
}

// The game's text becomes `text`: with the cursor at its end, the part from the first character that differs is erased and the new part typed,
// as the player would. The keyboard dialog takes it at once; the game's own text field takes it, in this order, when the game next updates it.
void SetGameText(Session session, const ImeCommon::HostSnapshot& snapshot, std::u16string_view text) {
	const std::u16string_view old = snapshot.text;
	size_t                    same = 0;
	while (same < old.size() && same < text.size() && old[same] == text[same]) {
		same++;
	}
	const int to_end = static_cast<int>(old.size()) - static_cast<int>(snapshot.cursor);
	if (session.source == Source::Dialog) {
		if (to_end != 0) {
			DialogIme::HostMoveCursor(session.generation, to_end);
		}
		for (size_t i = same; i < old.size(); i++) {
			DialogIme::HostBackspace(session.generation);
		}
		if (same < text.size()) {
			DialogIme::HostInsertText(session.generation, text.substr(same));
		}
		return;
	}
	for (int i = 0; i < to_end; i++) {
		ImeCommon::ExternalInput input;
		input.action = ImeCommon::ExternalAction::MoveRight;
		CoreIme::HostQueueExternalInput(session.generation, std::move(input));
	}
	for (size_t i = same; i < old.size(); i++) {
		CoreIme::HostBackspace(session.generation);
	}
	if (same < text.size()) {
		CoreIme::HostInsertText(session.generation, text.substr(same));
	}
}

// Done: the box's text goes to the game, which then closes its keyboard. Cancel: the game keeps its text.
void Finish(bool accept) {
	const auto open = OpenKeyboard();
	if (!open || open->first != g_keyboard.session) {
		return;
	}
	const auto& [session, snapshot] = *open;
	Log("keyboard: %s\n", accept ? "done" : "canceled");
	if (accept) {
		SetGameText(session, snapshot, ToU16(g_keyboard.text.Text()));
	}
	const auto finish = session.source == Source::Dialog ? (accept ? DialogIme::HostAccept : DialogIme::HostCancel)
	                                                     : (accept ? CoreIme::HostAccept : CoreIme::HostCancel);
	finish(session.generation);
}

} // namespace

void SetUpGameKeyboard(xaml::Controls::Grid const& page, uint32_t index, std::function<void(bool shown)> shown) {
	auto& keyboard = g_keyboard;
	keyboard.root  = xaml::Markup::XamlReader::Load(KeyboardMarkup).as<xaml::Controls::Grid>();
	keyboard.title = keyboard.root.FindName(L"KeyboardTitle").as<xaml::Controls::TextBlock>();
	keyboard.text  = keyboard.root.FindName(L"KeyboardText").as<xaml::Controls::TextBox>();
	keyboard.done  = keyboard.root.FindName(L"KeyboardDone").as<xaml::Controls::Button>();
	keyboard.shown = std::move(shown);
	SetHints(keyboard.root.FindName(L"KeyboardHints").as<xaml::Controls::Border>(), {{XboxButton::A, L"Select"}, {XboxButton::B, L"Cancel"}});
	page.Children().InsertAt(index, keyboard.root);

	keyboard.done.Click([](auto const&, auto const&) { Finish(true); });
	keyboard.text.KeyDown([](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
			args.Handled(true);
			Finish(true);
		}
	});
	keyboard.root.KeyDown([](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		if (args.Key() == winrt::Windows::System::VirtualKey::GamepadB || args.Key() == winrt::Windows::System::VirtualKey::Escape) {
			args.Handled(true);
			Finish(false);
		}
	});

	g_dispatcher = page.Dispatcher();
	CoreIme::SetVisibilityCallback(OnVisibilityChanged);
	DialogIme::SetVisibilityCallback(OnVisibilityChanged);
	Refresh();
}

bool GameKeyboardShown() {
	return g_keyboard.session.source != Source::None;
}

} // namespace Kyty::Uwp
