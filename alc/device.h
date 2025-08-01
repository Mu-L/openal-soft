#ifndef ALC_DEVICE_H
#define ALC_DEVICE_H

#include "config.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"

#include "alconfig.h"
#include "core/device.h"
#include "intrusive_ptr.h"

#if ALSOFT_EAX
#include "al/eax/x_ram.h"
#endif // ALSOFT_EAX

struct BackendBase;
struct BufferSubList;
struct EffectSubList;
struct FilterSubList;

using uint = unsigned int;


struct ALCdevice {
    ALCdevice(const ALCdevice&) = delete;
    auto operator=(const ALCdevice&) -> ALCdevice& = delete;
    virtual ~ALCdevice() = default;
protected:
    ALCdevice() = default;
};

namespace al {

struct Device final : public ALCdevice, al::intrusive_ref<al::Device>, DeviceBase {
    /* This lock protects the device state (format, update size, etc) from
     * being from being changed in multiple threads, or being accessed while
     * being changed. It's also used to serialize calls to the backend.
     */
    std::mutex StateLock;
    std::unique_ptr<BackendBase> Backend;

    ALCuint NumMonoSources{};
    ALCuint NumStereoSources{};

    // Maximum number of sources that can be created
    uint SourcesMax{};
    // Maximum number of slots that can be created
    uint AuxiliaryEffectSlotMax{};

    std::string mHrtfName;
    std::vector<std::string> mHrtfList;
    ALCenum mHrtfStatus{ALC_FALSE};

    enum class OutputMode1 : ALCenum {
        Any = ALC_ANY_SOFT,
        Mono = ALC_MONO_SOFT,
        Stereo = ALC_STEREO_SOFT,
        StereoBasic = ALC_STEREO_BASIC_SOFT,
        Uhj2 = ALC_STEREO_UHJ_SOFT,
        Hrtf = ALC_STEREO_HRTF_SOFT,
        Quad = ALC_QUAD_SOFT,
        X51 = ALC_SURROUND_5_1_SOFT,
        X61 = ALC_SURROUND_6_1_SOFT,
        X71 = ALC_SURROUND_7_1_SOFT
    };
    OutputMode1 getOutputMode1() const noexcept;

    using OutputMode = OutputMode1;

    std::atomic<ALCenum> mLastError{ALC_NO_ERROR};

    // Map of Buffers for this device
    std::mutex BufferLock;
    std::vector<BufferSubList> BufferList;

    // Map of Effects for this device
    std::mutex EffectLock;
    std::vector<EffectSubList> EffectList;

    // Map of Filters for this device
    std::mutex FilterLock;
    std::vector<FilterSubList> FilterList;

#if ALSOFT_EAX
    ALuint eax_x_ram_free_size{eax_x_ram_max_size};
#endif // ALSOFT_EAX


    std::unordered_map<ALuint,std::string> mBufferNames;
    std::unordered_map<ALuint,std::string> mEffectNames;
    std::unordered_map<ALuint,std::string> mFilterNames;

    std::string mVendorOverride;
    std::string mVersionOverride;
    std::string mRendererOverride;

    explicit Device(DeviceType type);
    ~Device() final;

    void enumerateHrtfs();

    bool getConfigValueBool(const std::string_view block, const std::string_view key, bool def)
    { return GetConfigValueBool(mDeviceName, block, key, def); }

    template<typename T>
    auto configValue(const std::string_view block, const std::string_view key) -> std::optional<T> = delete;

    /** Stores the latest ALC device error. */
    static void SetGlobalError(ALCenum errorCode) { SetError(nullptr, errorCode); }
    void setError(ALCenum errorCode) { SetError(this, errorCode); }

    static inline std::atomic<ALCenum> sLastGlobalError{ALC_NO_ERROR};
    /* Flag to trap ALC device errors */
    static inline bool sTrapALCError{false};

private:
    static void SetError(Device *device, ALCenum errorCode);
};

template<> inline
auto Device::configValue(const std::string_view block, const std::string_view key) -> std::optional<std::string>
{ return ConfigValueStr(mDeviceName, block, key); }
template<> inline
auto Device::configValue(const std::string_view block, const std::string_view key) -> std::optional<int>
{ return ConfigValueInt(mDeviceName, block, key); }
template<> inline
auto Device::configValue(const std::string_view block, const std::string_view key) -> std::optional<uint>
{ return ConfigValueUInt(mDeviceName, block, key); }
template<> inline
auto Device::configValue(const std::string_view block, const std::string_view key) -> std::optional<float>
{ return ConfigValueFloat(mDeviceName, block, key); }
template<> inline
auto Device::configValue(const std::string_view block, const std::string_view key) -> std::optional<bool>
{ return ConfigValueBool(mDeviceName, block, key); }

} // namespace al

#endif
