#include "systemDialog.h"

#include "log.h"
#include "xboxLook.h"
#include "libs/dialog.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>

#include <cstdio>
#include <optional>
#include <utility>

namespace Kyty::Uwp {

namespace {

namespace xaml         = winrt::Windows::UI::Xaml;
namespace SystemDialog = Libs::Dialog::SystemDialog;

// In the middle of the screen, the game dimmed behind it.
constexpr wchar_t DialogMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      Visibility="Collapsed">
  <Rectangle Fill="{ThemeResource SystemControlPageBackgroundMediumAltMediumBrush}" />
  <Grid Width="560" HorizontalAlignment="Center" VerticalAlignment="Center" Padding="24"
        Background="{ThemeResource SystemControlBackgroundChromeMediumLowBrush}">
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto" />
      <RowDefinition Height="Auto" />
      <RowDefinition Height="Auto" />
      <RowDefinition Height="Auto" />
    </Grid.RowDefinitions>
    <TextBlock x:Name="DialogTitle" Style="{StaticResource SubtitleTextBlockStyle}" Margin="0,0,0,16" />
    <TextBlock x:Name="DialogText" Grid.Row="1" Style="{StaticResource BodyTextBlockStyle}" TextWrapping="Wrap" />
    <TextBlock x:Name="DialogCode" Grid.Row="2" Style="{StaticResource CaptionTextBlockStyle}" Margin="0,8,0,0"
               Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
    <Grid Grid.Row="3" Margin="0,24,0,0">
      <Border x:Name="DialogHints" Style="{StaticResource HintBarStyle}" HorizontalAlignment="Left" VerticalAlignment="Center" />
      <Button x:Name="DialogButton" HorizontalAlignment="Right" MinWidth="120" Style="{StaticResource AccentButtonStyle}" />
    </Grid>
  </Grid>
</Grid>)xaml";

struct Box {
	xaml::Controls::Grid      root {nullptr};
	xaml::Controls::TextBlock title {nullptr};
	xaml::Controls::TextBlock text {nullptr};
	xaml::Controls::TextBlock code {nullptr};
	xaml::Controls::Border    hints {nullptr};
	xaml::Controls::Button    button {nullptr};
	std::function<void(bool)> shown;
	// The dialog that shows, by the time it opened (0: none).
	uint64_t generation = 0;
	bool     signin     = false;
};

// UI thread only, but the dispatcher: the visibility callback runs on the game's threads and posts to it; it is set before it is.
Box                                      g_box;
winrt::Windows::UI::Core::CoreDispatcher g_dispatcher {nullptr};

// The texts are the desktop overlay's (graphics/presentation/systemOverlay.cpp).
void Show(const SystemDialog::HostSnapshot& snapshot) {
	auto&      box    = g_box;
	const bool signin = snapshot.kind == SystemDialog::Kind::Signin;
	const auto code   = static_cast<uint32_t>(snapshot.error_code);
	box.signin        = signin;
	box.title.Text(signin ? L"Sign in to PlayStation Network" : L"Error");
	box.text.Text(signin                ? L"PlayStation Network sign-in is unavailable."
	              : code == 0x80550006u ? L"You are not signed in to PlayStation Network."
	                                    : L"An error has occurred.");
	wchar_t code_text[32] {};
	std::swprintf(code_text, std::size(code_text), L"Error code: 0x%08X", code);
	box.code.Text(code_text);
	box.code.Visibility(signin ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
	box.button.Content(winrt::box_value(signin ? L"Cancel" : L"OK"));
	if (signin) {
		SetHints(box.hints, {{XboxButton::A, L"Select"}, {XboxButton::B, L"Cancel"}});
	} else {
		SetHints(box.hints, {{XboxButton::A, L"Select"}});
	}
	box.root.Visibility(xaml::Visibility::Visible);
	box.root.UpdateLayout();
	box.button.Focus(xaml::FocusState::Programmatic);
	Log("system dialog: %s (0x%08x)\n", signin ? "PlayStation Network sign-in" : "error", code);
	if (box.shown) {
		box.shown(true);
	}
}

void Hide() {
	g_box.root.Visibility(xaml::Visibility::Collapsed);
	Log("system dialog: closed\n");
	if (g_box.shown) {
		g_box.shown(false);
	}
}

// Shows the dialog the game has open now, or hides the box when it has none.
void Refresh() {
	SystemDialog::HostSnapshot snapshot {};
	const bool                 open       = SystemDialog::GetHostSnapshot(&snapshot);
	const uint64_t             generation = open ? snapshot.generation : 0;
	if (generation == g_box.generation) {
		return;
	}
	const bool was_shown = g_box.generation != 0;
	g_box.generation     = generation;
	if (open) {
		Show(snapshot);
	} else if (was_shown) {
		Hide();
	}
}

void OnVisibilityChanged() {
	if (g_dispatcher != nullptr) {
		(void)g_dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [] { Refresh(); });
	}
}

// The button: the game is told the dialog closed (and the box goes with the next refresh).
void Close() {
	if (g_box.generation != 0 && SystemDialog::HostClose(g_box.generation)) {
		Log("system dialog: %s\n", g_box.signin ? "canceled" : "ok");
	}
	Refresh();
}

} // namespace

void SetUpSystemDialog(xaml::Controls::Grid const& page, uint32_t index, std::function<void(bool shown)> shown) {
	auto& box  = g_box;
	box.root   = xaml::Markup::XamlReader::Load(DialogMarkup).as<xaml::Controls::Grid>();
	box.title  = box.root.FindName(L"DialogTitle").as<xaml::Controls::TextBlock>();
	box.text   = box.root.FindName(L"DialogText").as<xaml::Controls::TextBlock>();
	box.code   = box.root.FindName(L"DialogCode").as<xaml::Controls::TextBlock>();
	box.hints  = box.root.FindName(L"DialogHints").as<xaml::Controls::Border>();
	box.button = box.root.FindName(L"DialogButton").as<xaml::Controls::Button>();
	box.shown  = std::move(shown);
	page.Children().InsertAt(index, box.root);

	box.button.Click([](auto const&, auto const&) { Close(); });
	// B closes the sign-in as its Cancel does (as on the desktop); an error closes with its OK only.
	box.root.KeyDown([](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		const auto key = args.Key();
		if (g_box.signin && (key == winrt::Windows::System::VirtualKey::GamepadB || key == winrt::Windows::System::VirtualKey::Escape)) {
			args.Handled(true);
			Close();
		}
	});

	g_dispatcher = page.Dispatcher();
	SystemDialog::SetVisibilityCallback(OnVisibilityChanged);
	Refresh();
}

bool SystemDialogShown() {
	return g_box.generation != 0;
}

} // namespace Kyty::Uwp
