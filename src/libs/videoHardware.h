#ifndef EMULATOR_SRC_LIBS_VIDEOHARDWARE_H_
#define EMULATOR_SRC_LIBS_VIDEOHARDWARE_H_

struct AVBufferRef;
struct AVCodecContext;
struct ID3D12Device;

// Hardware video decoding for FFmpeg's decoders: D3D12 video on the D3D12 renderer's own device, shared by every player (FFmpeg locks
// it for each use). Windows with the D3D12 renderer only; elsewhere videos are decoded on the CPU.
namespace Libs::VideoHardware {

// Called by the D3D12 renderer once it has made its device, on the emulator's thread: the device becomes an FFmpeg hardware device
// context (AV_HWDEVICE_TYPE_D3D12VA). When that fails the reason is logged once and videos are decoded on the CPU.
void UseDevice(ID3D12Device* device);

// The device context once the renderer has handed its device over, else nullptr. Callers take their own reference (av_buffer_ref).
// The decoder's D3D12 calls run on a thread of the app's own: on a game thread, whose stack bounds are cleared while it runs game
// code (Pthread), an exception the Xbox driver raises and catches inside its debug output ends the app.
AVBufferRef* Device();

// From FFmpeg's get_format callback when it takes D3D12 frames: the decoder's frame pool, at the coded size. FFmpeg makes D3D12 frames
// at the display size (1080 rows for a 1080p H.264 video), and the Xbox's decoder writes nothing into a texture smaller than the coded
// picture (1088 rows); its D3D11 path rounds up the same way. False: FFmpeg makes its own pool.
bool MakeFrames(AVCodecContext* c);

} // namespace Libs::VideoHardware

#endif // EMULATOR_SRC_LIBS_VIDEOHARDWARE_H_
