#ifndef EMULATOR_SRC_UWP_TITLEMUSIC_H_
#define EMULATOR_SRC_UWP_TITLEMUSIC_H_

#include "gameLibrary.h"

// A game's music in the library, as the PS4 plays it on its home screen: sce_sys/snd0.at9 (ATRAC9 in a RIFF file), looped where the file says,
// fading in a moment after its game gets the focus. For a game without it, the log names the sound files its sce_sys has.
namespace Kyty::Uwp::TitleMusic {

// UI thread: plays this game's music after a moment; the music playing fades out. Moving on before the moment is over plays nothing.
void Play(const GameEntry& game);

// The music fades out: the library goes, or a game starts.
void Stop();

} // namespace Kyty::Uwp::TitleMusic

#endif // EMULATOR_SRC_UWP_TITLEMUSIC_H_
