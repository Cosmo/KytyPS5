#ifndef EMULATOR_SRC_UWP_APP_H_
#define EMULATOR_SRC_UWP_APP_H_

#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.h>

namespace Kyty::Uwp {

// The XAML application. It resolves WinUI 2 types for markup loaded at runtime through WinUI's
// metadata provider, so no XAML compiler is involved.
struct App: winrt::Windows::UI::Xaml::ApplicationT<App,
                                                    winrt::Windows::UI::Xaml::Markup::IXamlMetadataProvider> {
	App();

	void OnLaunched(winrt::Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const& args);
	void OnActivated(winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs const& args);

	winrt::Windows::UI::Xaml::Markup::IXamlType
	GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type);
	winrt::Windows::UI::Xaml::Markup::IXamlType GetXamlType(winrt::hstring const& full_name);
	winrt::com_array<winrt::Windows::UI::Xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions();

private:
	void Show(winrt::hstring const& arguments);

	winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider m_winui_types;
	bool                                                                       m_resources_loaded = false;
};

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_APP_H_
