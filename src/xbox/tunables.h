#ifndef KYTY_XBOX_TUNABLES_H_
#define KYTY_XBOX_TUNABLES_H_

// Switches of the Xbox changes.

#include <cstdlib>

namespace Xbox::Tunables {

// T3: direct and flexible memory are lazily committed private regions instead of views of one committed section.
// Always on in the UWP app (the console cannot create the section). Elsewhere it is off unless the environment variable KYTY_LAZY_MEMORY is
// set to something other than 0, so that the desktop build keeps its behavior.
inline bool LazyMemory() {
#if defined(KYTY_PLATFORM_UWP)
	return true;
#else
	static const bool on = [] {
		const char* value = std::getenv("KYTY_LAZY_MEMORY");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return on;
#endif
}

} // namespace Xbox::Tunables

#endif
