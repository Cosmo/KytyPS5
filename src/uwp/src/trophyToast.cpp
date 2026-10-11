#include "trophyToast.h"

#include "log.h"

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>

#include <array>
#include <cstdint>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace dom           = winrt::Windows::Data::Xml::Dom;
namespace notifications = winrt::Windows::UI::Notifications;

const char* SettingName(notifications::NotificationSetting setting) {
	switch (setting) {
		case notifications::NotificationSetting::Enabled: return "enabled";
		case notifications::NotificationSetting::DisabledForApplication: return "disabled for the app";
		case notifications::NotificationSetting::DisabledForUser: return "disabled for the user";
		case notifications::NotificationSetting::DisabledByGroupPolicy: return "disabled by policy";
		case notifications::NotificationSetting::DisabledByManifest: return "disabled by the manifest";
		default: return "unknown";
	}
}

// The last notifications, kept so that their events still arrive.
constexpr size_t                               RECENT_NOTIFICATIONS = 8;
std::vector<notifications::ToastNotification> g_recent;

// Shows the notification and logs what becomes of it: whether the system allows the app's notifications, and the Failed and Dismissed events
// (the system may refuse a notification after Show returned).
void Send(const dom::XmlDocument& document, const std::string& what) {
	const auto notifier = notifications::ToastNotificationManager::CreateToastNotifier();
	Log("%s: notifications %s\n", what.c_str(), SettingName(notifier.Setting()));
	notifications::ToastNotification toast(document);
	toast.Failed([what](auto const&, notifications::ToastFailedEventArgs const& args) {
		Log("%s: failed, error 0x%08X\n", what.c_str(), static_cast<uint32_t>(args.ErrorCode().value));
	});
	toast.Dismissed([what](auto const&, notifications::ToastDismissedEventArgs const& args) {
		Log("%s: dismissed, reason %d (0 by the user, 1 hidden by the app, 2 timed out)\n", what.c_str(), static_cast<int>(args.Reason()));
	});
	notifier.Show(toast);
	g_recent.push_back(toast);
	if (g_recent.size() > RECENT_NOTIFICATIONS) {
		g_recent.erase(g_recent.begin());
	}
	Log("%s: sent\n", what.c_str());
}

} // namespace

void ShowTrophyToast(const std::string& name, const std::wstring& link) {
	const std::string what = "trophy notification (" + name + ")";
	try {
		// The text goes in as text nodes and the link as an attribute value, so that names with & or < need no escaping.
		dom::XmlDocument document;
		document.LoadXml(L"<toast><visual><binding template=\"ToastGeneric\"><text/><text/></binding></visual></toast>");
		document.DocumentElement().SetAttribute(L"launch", link);
		const std::array<winrt::hstring, 2> lines = {winrt::to_hstring(name), L"Trophy earned!"};
		const auto                          texts = document.GetElementsByTagName(L"text");
		for (uint32_t index = 0; index < lines.size(); index++) {
			texts.Item(index).AppendChild(document.CreateTextNode(lines[index]));
		}
		Send(document, what);
	} catch (const winrt::hresult_error& error) {
		Log("%s failed: 0x%08X %ls\n", what.c_str(), static_cast<uint32_t>(error.code().value), error.message().c_str());
	}
}

} // namespace Kyty::Uwp
