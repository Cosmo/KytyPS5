#include "app.h"
#include "log.h"

#include <windows.h>

int __stdcall wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, PWSTR /*command_line*/,
                       int /*show*/) {
	try {
		winrt::init_apartment();
		Kyty::Uwp::Log("KytyPS5 UWP starting\n");
		winrt::Windows::UI::Xaml::Application::Start(
		    [](auto&&) { winrt::make<Kyty::Uwp::App>(); });
	} catch (const winrt::hresult_error& error) {
		Kyty::Uwp::Log("fatal: 0x%08x %ls\n", static_cast<uint32_t>(error.code()),
		               error.message().c_str());
		return 1;
	}
	return 0;
}
