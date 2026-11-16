#ifndef EMULATOR_SRC_UWP_FOLDERPANEL_H_
#define EMULATOR_SRC_UWP_FOLDERPANEL_H_

#include <winrt/Windows.UI.Xaml.Controls.h>

#include <filesystem>
#include <functional>
#include <string>

// Choosing a folder in the Xbox's Development Files (D:\DevelopmentFiles, which the Device Portal's File explorer copies files into and
// which the system's folder picker doesn't show): a panel from the right over the page, as the library's game options. A opens a folder and
// B goes back up; "Use this folder" chooses the folder shown.
namespace Kyty::Uwp {

struct FolderPanelOptions {
	std::wstring                                      caption;            // over the folder's name: what the folder is for
	bool                                              count_games = true; // how many games a scan finds, next to each folder
	std::function<void(const std::filesystem::path&)> chosen;             // "Use this folder"
	std::function<void()>                             closed;             // after the panel went, chosen or not
};

// Whether the app may read Development Files, and write in it (only the Xbox has it).
[[nodiscard]] bool DevelopmentFilesReadable();
[[nodiscard]] bool DevelopmentFilesWritable();
// Whether the C library may write in it, as the logs and the frame dumps need.
[[nodiscard]] bool DevelopmentFilesDirectlyWritable();

// Shows the panel over `page` (spanning its rows and columns), at Development Files. The result is for the page's Back (B, Escape and the
// window's back button go to the panel while it has the focus; this is for the rest): it goes up a level or closes the panel, and is false
// once the panel is gone.
std::function<bool()> ShowDevelopmentFilesPanel(winrt::Windows::UI::Xaml::Controls::Grid const& page, FolderPanelOptions options);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_FOLDERPANEL_H_
