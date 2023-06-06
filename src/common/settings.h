// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <forward_list>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/settings_input.h"

namespace Settings {

enum class AstcDecodeMode : u32 {
    CPU = 0,
    GPU = 1,
    CPUAsynchronous = 2,
};

enum class VSyncMode : u32 {
    Immediate = 0,
    Mailbox = 1,
    FIFO = 2,
    FIFORelaxed = 3,
};

enum class RendererBackend : u32 {
    OpenGL = 0,
    Vulkan = 1,
    Null = 2,
};

enum class ShaderBackend : u32 {
    GLSL = 0,
    GLASM = 1,
    SPIRV = 2,
};

enum class GPUAccuracy : u32 {
    Normal = 0,
    High = 1,
    Extreme = 2,
};

enum class CPUAccuracy : u32 {
    Auto = 0,
    Accurate = 1,
    Unsafe = 2,
    Paranoid = 3,
};

enum class FullscreenMode : u32 {
    Borderless = 0,
    Exclusive = 1,
};

enum class NvdecEmulation : u32 {
    Off = 0,
    CPU = 1,
    GPU = 2,
};

enum class ResolutionSetup : u32 {
    Res1_2X = 0,
    Res3_4X = 1,
    Res1X = 2,
    Res3_2X = 3,
    Res2X = 4,
    Res3X = 5,
    Res4X = 6,
    Res5X = 7,
    Res6X = 8,
    Res7X = 9,
    Res8X = 10,
};

enum class ScalingFilter : u32 {
    NearestNeighbor = 0,
    Bilinear = 1,
    Bicubic = 2,
    Gaussian = 3,
    ScaleForce = 4,
    Fsr = 5,
    LastFilter = Fsr,
};

enum class AntiAliasing : u32 {
    None = 0,
    Fxaa = 1,
    Smaa = 2,
    LastAA = Smaa,
};

enum class AstcRecompression : u32 {
    Uncompressed = 0,
    Bc1 = 1,
    Bc3 = 2,
};

enum class Category : u32 {
    Audio,
    Core,
    Cpu,
    Renderer,
    AdvancedGraphics,
    System,
    DataStorage,
    Debugging,
    Miscellaneous,
    Network,
    WebService,
    AddOns,
    Controls,
    Ui,
    UiLayout,
    UiGameList,
    Screenshots,
    Shortcuts,
    Multiplayer,
    Services,
    Paths,
    MaxEnum,
};

const char* TranslateCategory(Settings::Category category);

struct ResolutionScalingInfo {
    u32 up_scale{1};
    u32 down_shift{0};
    f32 up_factor{1.0f};
    f32 down_factor{1.0f};
    bool active{};
    bool downscale{};

    s32 ScaleUp(s32 value) const {
        if (value == 0) {
            return 0;
        }
        return std::max((value * static_cast<s32>(up_scale)) >> static_cast<s32>(down_shift), 1);
    }

    u32 ScaleUp(u32 value) const {
        if (value == 0U) {
            return 0U;
        }
        return std::max((value * up_scale) >> down_shift, 1U);
    }
};

class BasicSetting {
protected:
    explicit BasicSetting() = default;

public:
    virtual ~BasicSetting() = default;

    virtual Category Category() const = 0;
    virtual constexpr bool Switchable() const = 0;
    virtual std::string ToString() const = 0;
    virtual void LoadString(const std::string& load) = 0;
    virtual const std::string& GetLabel() const = 0;
    virtual std::string DefaultToString() const = 0;
    virtual bool Save() const = 0;
    virtual std::type_index TypeId() const = 0;
    virtual void SetGlobal(bool global) {}
    virtual bool UsingGlobal() const {
        return false;
    }
};

class Linkage {
public:
    std::map<Category, std::forward_list<BasicSetting*>> by_category;
    std::vector<std::function<void()>> restore_functions;
};

/** The Setting class is a simple resource manager. It defines a label and default value
 * alongside the actual value of the setting for simpler and less-error prone use with frontend
 * configurations. Specifying a default value and label is required. A minimum and maximum range
 * can be specified for sanitization.
 */
template <typename Type, bool ranged = false, bool save = true>
class Setting : public BasicSetting {
protected:
    Setting() = default;

    /**
     * Only sets the setting to the given initializer, leaving the other members to their default
     * initializers.
     *
     * @param global_val Initial value of the setting
     */
    explicit Setting(const Type& val) : value{val} {}

public:
    /**
     * Sets a default value, label, and setting value.
     *
     * @param linkage Setting registry
     * @param default_val Initial value of the setting, and default value of the setting
     * @param name Label for the setting
     * @param category_ Category of the setting AKA INI group
     */
    explicit Setting(Linkage& linkage, const Type& default_val, const std::string& name,
                     enum Category category_)
        requires(!ranged)
        : value{default_val}, default_value{default_val}, label{name}, category{category_} {
        linkage.by_category[category].push_front(this);
    }
    virtual ~Setting() = default;

    /**
     * Sets a default value, minimum value, maximum value, and label.
     *
     * @param linkage Setting registry
     * @param default_val Initial value of the setting, and default value of the setting
     * @param min_val Sets the minimum allowed value of the setting
     * @param max_val Sets the maximum allowed value of the setting
     * @param name Label for the setting
     * @param category_ Category of the setting AKA INI group
     */
    explicit Setting(Linkage& linkage, const Type& default_val, const Type& min_val,
                     const Type& max_val, const std::string& name, enum Category category_)
        requires(ranged)
        : value{default_val}, default_value{default_val}, maximum{max_val}, minimum{min_val},
          label{name}, category{category_} {
        linkage.by_category[category].push_front(this);
    }

    /**
     *  Returns a reference to the setting's value.
     *
     * @returns A reference to the setting
     */
    [[nodiscard]] virtual const Type& GetValue() const {
        return value;
    }

    /**
     * Sets the setting to the given value.
     *
     * @param val The desired value
     */
    virtual void SetValue(const Type& val) {
        Type temp{ranged ? std::clamp(val, minimum, maximum) : val};
        std::swap(value, temp);
    }

    /**
     * Returns the value that this setting was created with.
     *
     * @returns A reference to the default value
     */
    [[nodiscard]] const Type& GetDefault() const {
        return default_value;
    }

    /**
     * Returns the label this setting was created with.
     *
     * @returns A reference to the label
     */
    [[nodiscard]] const std::string& GetLabel() const override {
        return label;
    }

    /**
     * Returns the setting's category AKA INI group.
     *
     * @returns The setting's category
     */
    [[nodiscard]] enum Category Category() const override {
        return category;
    }

    /**
     * Returns whether the current setting is Switchable.
     *
     * @returns If the setting is a SwitchableSetting
     */
    [[nodiscard]] virtual constexpr bool Switchable() const override {
        return false;
    }

private:
    std::string ToString(const Type& value_) const {
        if constexpr (std::is_same<Type, std::string>()) {
            return value_;
        } else if constexpr (std::is_same<Type, std::optional<u32>>()) {
            return value_.has_value() ? std::to_string(*value_) : "0";
        } else if constexpr (std::is_same<Type, bool>()) {
            return value_ ? "true" : "false";
        } else {
            return std::to_string(static_cast<u64>(value_));
        }
    }

public:
    /**
     * Converts the value of the setting to a std::string. Respects the global state if the setting
     * has one.
     *
     * @returns The current setting as a std::string
     */
    std::string ToString() const override {
        return ToString(this->GetValue());
    }

    /**
     * Returns the default value of the setting as a std::string.
     *
     * @returns The default value as a string.
     */
    std::string DefaultToString() const override {
        return ToString(default_value);
    }

    /**
     * Assigns a value to the setting.
     *
     * @param val The desired setting value
     *
     * @returns A reference to the setting
     */
    virtual const Type& operator=(const Type& val) {
        Type temp{ranged ? std::clamp(val, minimum, maximum) : val};
        std::swap(value, temp);
        return value;
    }

    /**
     * Returns a reference to the setting.
     *
     * @returns A reference to the setting
     */
    explicit virtual operator const Type&() const {
        return value;
    }

    /**
     * Converts the given value to the Setting's type of value. Uses SetValue to enter the setting,
     * thus respecting its constraints.
     *
     * @param input The desired value
     */
    void LoadString(const std::string& input) override {
        if (input.empty()) {
            this->SetValue(this->GetDefault());
            return;
        }
        try {
            if constexpr (std::is_same<Type, std::string>()) {
                this->SetValue(input);
            } else if constexpr (std::is_same<Type, std::optional<u32>>()) {
                this->SetValue(static_cast<u32>(std::stoll(input)));
            } else if constexpr (std::is_same<Type, bool>()) {
                this->SetValue(input == "true");
            } else {
                this->SetValue(static_cast<Type>(std::stoll(input)));
            }
        } catch (std::invalid_argument) {
            this->SetValue(this->GetDefault());
        }
    }

    /**
     * Returns the save preference of the setting i.e. when saving or reading the setting from a
     * frontend, whether this setting should be skipped.
     *
     * @returns The save preference
     */
    virtual bool Save() const override {
        return save;
    }

    /**
     * Gives us another way to identify the setting without having to go through a string.
     *
     * @returns the type_index of the setting's type
     */
    virtual std::type_index TypeId() const override {
        return std::type_index(typeid(Type));
    }

protected:
    Type value{};                 ///< The setting
    const Type default_value{};   ///< The default value
    const Type maximum{};         ///< Maximum allowed value of the setting
    const Type minimum{};         ///< Minimum allowed value of the setting
    const std::string label{};    ///< The setting's label
    const enum Category category; ///< The setting's category AKA INI group
};

/**
 * The SwitchableSetting class is a slightly more complex version of the Setting class. This adds a
 * custom setting to switch to when a guest application specifically requires it. The effect is that
 * other components of the emulator can access the setting's intended value without any need for the
 * component to ask whether the custom or global setting is needed at the moment.
 *
 * By default, the global setting is used.
 */
template <typename Type, bool ranged = false, bool save = true>
class SwitchableSetting : virtual public Setting<Type, ranged, save> {
public:
    /**
     * Sets a default value, label, and setting value.
     *
     * @param linkage Setting registry
     * @param default_val Initial value of the setting, and default value of the setting
     * @param name Label for the setting
     * @param category_ Category of the setting AKA INI group
     */
    explicit SwitchableSetting(Linkage& linkage, const Type& default_val, const std::string& name,
                               Category category)
        requires(!ranged)
        : Setting<Type, false, save>{linkage, default_val, name, category} {
        linkage.restore_functions.emplace_back([this]() { this->SetGlobal(true); });
    }
    virtual ~SwitchableSetting() = default;

    /**
     * Sets a default value, minimum value, maximum value, and label.
     *
     * @param linkage Setting registry
     * @param default_val Initial value of the setting, and default value of the setting
     * @param min_val Sets the minimum allowed value of the setting
     * @param max_val Sets the maximum allowed value of the setting
     * @param name Label for the setting
     * @param category_ Category of the setting AKA INI group
     */
    explicit SwitchableSetting(Linkage& linkage, const Type& default_val, const Type& min_val,
                               const Type& max_val, const std::string& name, Category category)
        requires(ranged)
        : Setting<Type, true, save>{linkage, default_val, min_val, max_val, name, category} {
        linkage.restore_functions.emplace_back([this]() { this->SetGlobal(true); });
    }

    /**
     * Tells this setting to represent either the global or custom setting when other member
     * functions are used.
     *
     * @param to_global Whether to use the global or custom setting.
     */
    void SetGlobal(bool to_global) override {
        use_global = to_global;
    }

    /**
     * Returns whether this setting is using the global setting or not.
     *
     * @returns The global state
     */
    [[nodiscard]] bool UsingGlobal() const override {
        return use_global;
    }

    /**
     * Returns either the global or custom setting depending on the values of this setting's global
     * state or if the global value was specifically requested.
     *
     * @param need_global Request global value regardless of setting's state; defaults to false
     *
     * @returns The required value of the setting
     */
    [[nodiscard]] virtual const Type& GetValue() const override {
        if (use_global) {
            return this->value;
        }
        return custom;
    }
    [[nodiscard]] virtual const Type& GetValue(bool need_global) const {
        if (use_global || need_global) {
            return this->value;
        }
        return custom;
    }

    /**
     * Sets the current setting value depending on the global state.
     *
     * @param val The new value
     */
    void SetValue(const Type& val) override {
        Type temp{ranged ? std::clamp(val, this->minimum, this->maximum) : val};
        if (use_global) {
            std::swap(this->value, temp);
        } else {
            std::swap(custom, temp);
        }
    }

    [[nodiscard]] virtual constexpr bool Switchable() const override {
        return true;
    }

    /**
     * Assigns the current setting value depending on the global state.
     *
     * @param val The new value
     *
     * @returns A reference to the current setting value
     */
    const Type& operator=(const Type& val) override {
        Type temp{ranged ? std::clamp(val, this->minimum, this->maximum) : val};
        if (use_global) {
            std::swap(this->value, temp);
            return this->value;
        }
        std::swap(custom, temp);
        return custom;
    }

    /**
     * Returns the current setting value depending on the global state.
     *
     * @returns A reference to the current setting value
     */
    virtual explicit operator const Type&() const override {
        if (use_global) {
            return this->value;
        }
        return custom;
    }

protected:
    bool use_global{true}; ///< The setting's global state
    Type custom{};         ///< The custom value of the setting
};

/**
 * The InputSetting class allows for getting a reference to either the global or custom members.
 * This is required as we cannot easily modify the values of user-defined types within containers
 * using the SetValue() member function found in the Setting class. The primary purpose of this
 * class is to store an array of 10 PlayerInput structs for both the global and custom setting and
 * allows for easily accessing and modifying both settings.
 */
template <typename Type>
class InputSetting final {
public:
    InputSetting() = default;
    explicit InputSetting(Type val) : Setting<Type>(val) {}
    ~InputSetting() = default;
    void SetGlobal(bool to_global) {
        use_global = to_global;
    }
    [[nodiscard]] bool UsingGlobal() const {
        return use_global;
    }
    [[nodiscard]] Type& GetValue(bool need_global = false) {
        if (use_global || need_global) {
            return global;
        }
        return custom;
    }

private:
    bool use_global{true}; ///< The setting's global state
    Type global{};         ///< The setting
    Type custom{};         ///< The custom setting value
};

struct TouchFromButtonMap {
    std::string name;
    std::vector<std::string> buttons;
};

struct Values {
    Linkage linkage{};

    // Audio
    Setting<std::string> sink_id{linkage, "auto", "output_engine", Category::Audio};
    Setting<std::string> audio_output_device_id{linkage, "auto", "output_device", Category::Audio};
    Setting<std::string> audio_input_device_id{linkage, "auto", "input_device", Category::Audio};
    Setting<bool, false, false> audio_muted{linkage, false, "audio_muted", Category::Audio};
    SwitchableSetting<u8, true> volume{linkage, 100, 0, 200, "volume", Category::Audio};
    Setting<bool, false, false> dump_audio_commands{linkage, false, "dump_audio_commands",
                                                    Category::Audio};

    // Core
    SwitchableSetting<bool> use_multi_core{linkage, true, "use_multi_core", Category::Core};
    SwitchableSetting<bool> use_unsafe_extended_memory_layout{
        linkage, false, "use_unsafe_extended_memory_layout", Category::Core};

    // Cpu
    SwitchableSetting<CPUAccuracy, true> cpu_accuracy{linkage,           CPUAccuracy::Auto,
                                                      CPUAccuracy::Auto, CPUAccuracy::Paranoid,
                                                      "cpu_accuracy",    Category::Cpu};
    // TODO: remove cpu_accuracy_first_time, migration setting added 8 July 2021
    Setting<bool> cpu_accuracy_first_time{linkage, true, "cpu_accuracy_first_time", Category::Cpu};
    Setting<bool> cpu_debug_mode{linkage, false, "cpu_debug_mode", Category::Cpu};

    Setting<bool> cpuopt_page_tables{linkage, true, "cpuopt_page_tables", Category::Cpu};
    Setting<bool> cpuopt_block_linking{linkage, true, "cpuopt_block_linking", Category::Cpu};
    Setting<bool> cpuopt_return_stack_buffer{linkage, true, "cpuopt_return_stack_buffer",
                                             Category::Cpu};
    Setting<bool> cpuopt_fast_dispatcher{linkage, true, "cpuopt_fast_dispatcher", Category::Cpu};
    Setting<bool> cpuopt_context_elimination{linkage, true, "cpuopt_context_elimination",
                                             Category::Cpu};
    Setting<bool> cpuopt_const_prop{linkage, true, "cpuopt_const_prop", Category::Cpu};
    Setting<bool> cpuopt_misc_ir{linkage, true, "cpuopt_misc_ir", Category::Cpu};
    Setting<bool> cpuopt_reduce_misalign_checks{linkage, true, "cpuopt_reduce_misalign_checks",
                                                Category::Cpu};
    Setting<bool> cpuopt_fastmem{linkage, true, "cpuopt_fastmem", Category::Cpu};
    Setting<bool> cpuopt_fastmem_exclusives{linkage, true, "cpuopt_fastmem_exclusives",
                                            Category::Cpu};
    Setting<bool> cpuopt_recompile_exclusives{linkage, true, "cpuopt_recompile_exclusives",
                                              Category::Cpu};
    Setting<bool> cpuopt_ignore_memory_aborts{linkage, true, "cpuopt_ignore_memory_aborts",
                                              Category::Cpu};

    SwitchableSetting<bool> cpuopt_unsafe_unfuse_fma{linkage, true, "cpuopt_unsafe_unfuse_fma",
                                                     Category::Cpu};
    SwitchableSetting<bool> cpuopt_unsafe_reduce_fp_error{
        linkage, true, "cpuopt_unsafe_reduce_fp_error", Category::Cpu};
    SwitchableSetting<bool> cpuopt_unsafe_ignore_standard_fpcr{
        linkage, true, "cpuopt_unsafe_ignore_standard_fpcr", Category::Cpu};
    SwitchableSetting<bool> cpuopt_unsafe_inaccurate_nan{
        linkage, true, "cpuopt_unsafe_inaccurate_nan", Category::Cpu};
    SwitchableSetting<bool> cpuopt_unsafe_fastmem_check{
        linkage, true, "cpuopt_unsafe_fastmem_check", Category::Cpu};
    SwitchableSetting<bool> cpuopt_unsafe_ignore_global_monitor{
        linkage, true, "cpuopt_unsafe_ignore_global_monitor", Category::Cpu};

    // Renderer
    SwitchableSetting<RendererBackend, true> renderer_backend{
        linkage,   RendererBackend::Vulkan, RendererBackend::OpenGL, RendererBackend::Null,
        "backend", Category::Renderer};
    SwitchableSetting<bool> async_presentation{linkage, false, "async_presentation",
                                               Category::AdvancedGraphics};
    SwitchableSetting<bool> renderer_force_max_clock{linkage, false, "force_max_clock",
                                                     Category::AdvancedGraphics};
    Setting<bool> renderer_debug{linkage, false, "debug", Category::Renderer};
    Setting<bool> renderer_shader_feedback{linkage, false, "shader_feedback", Category::Renderer};
    Setting<bool> enable_nsight_aftermath{linkage, false, "nsight_aftermath", Category::Renderer};
    Setting<bool> disable_shader_loop_safety_checks{
        linkage, false, "disable_shader_loop_safety_checks", Category::Renderer};
    SwitchableSetting<int> vulkan_device{linkage, 0, "vulkan_device", Category::Renderer};

    ResolutionScalingInfo resolution_info{};
    SwitchableSetting<ResolutionSetup> resolution_setup{linkage, ResolutionSetup::Res1X,
                                                        "resolution_setup", Category::Renderer};
    SwitchableSetting<ScalingFilter> scaling_filter{linkage, ScalingFilter::Bilinear,
                                                    "scaling_filter", Category::Renderer};
    SwitchableSetting<int, true> fsr_sharpening_slider{
        linkage, 25, 0, 200, "fsr_sharpening_slider", Category::Renderer};
    SwitchableSetting<AntiAliasing> anti_aliasing{linkage, AntiAliasing::None, "anti_aliasing",
                                                  Category::Renderer};
    // *nix platforms may have issues with the borderless windowed fullscreen mode.
    // Default to exclusive fullscreen on these platforms for now.
    SwitchableSetting<FullscreenMode, true> fullscreen_mode{linkage,
#ifdef _WIN32
                                                            FullscreenMode::Borderless,
#else
                                                            FullscreenMode::Exclusive,
#endif
                                                            FullscreenMode::Borderless,
                                                            FullscreenMode::Exclusive,
                                                            "fullscreen_mode",
                                                            Category::Renderer};
    SwitchableSetting<int, true> aspect_ratio{linkage, 0, 0, 4, "aspect_ratio", Category::Renderer};
    SwitchableSetting<int, true> max_anisotropy{
        linkage, 0, 0, 5, "max_anisotropy", Category::AdvancedGraphics};
    SwitchableSetting<bool, false, false> use_speed_limit{linkage, true, "use_speed_limit",
                                                          Category::Renderer};
    SwitchableSetting<u16, true> speed_limit{linkage, 100,           0,
                                             9999,    "speed_limit", Category::Renderer};
    SwitchableSetting<bool> use_disk_shader_cache{linkage, true, "use_disk_shader_cache",
                                                  Category::Renderer};
    SwitchableSetting<GPUAccuracy, true> gpu_accuracy{
        linkage,        GPUAccuracy::High,         GPUAccuracy::Normal, GPUAccuracy::Extreme,
        "gpu_accuracy", Category::AdvancedGraphics};
    SwitchableSetting<bool> use_asynchronous_gpu_emulation{
        linkage, true, "use_asynchronous_gpu_emulation", Category::Renderer};
    SwitchableSetting<NvdecEmulation> nvdec_emulation{linkage, NvdecEmulation::GPU,
                                                      "nvdec_emulation", Category::Renderer};
    SwitchableSetting<AstcDecodeMode, true> accelerate_astc{linkage,
                                                            AstcDecodeMode::CPU,
                                                            AstcDecodeMode::CPU,
                                                            AstcDecodeMode::CPUAsynchronous,
                                                            "accelerate_astc",
                                                            Category::Renderer};
    Setting<VSyncMode, true> vsync_mode{
        linkage,     VSyncMode::FIFO,   VSyncMode::Immediate, VSyncMode::FIFORelaxed,
        "use_vsync", Category::Renderer};
    SwitchableSetting<bool> use_reactive_flushing{linkage, true, "use_reactive_flushing",
                                                  Category::Renderer};
    SwitchableSetting<ShaderBackend, true> shader_backend{
        linkage,          ShaderBackend::GLSL, ShaderBackend::GLSL, ShaderBackend::SPIRV,
        "shader_backend", Category::Renderer};
    SwitchableSetting<bool> use_asynchronous_shaders{linkage, false, "use_asynchronous_shaders",
                                                     Category::Renderer};
    SwitchableSetting<bool> use_fast_gpu_time{linkage, true, "use_fast_gpu_time",
                                              Category::AdvancedGraphics};
    SwitchableSetting<bool> use_vulkan_driver_pipeline_cache{
        linkage, true, "use_vulkan_driver_pipeline_cache", Category::AdvancedGraphics};
    SwitchableSetting<bool> enable_compute_pipelines{linkage, false, "enable_compute_pipelines",
                                                     Category::AdvancedGraphics};
    SwitchableSetting<AstcRecompression, true> astc_recompression{linkage,
                                                                  AstcRecompression::Uncompressed,
                                                                  AstcRecompression::Uncompressed,
                                                                  AstcRecompression::Bc3,
                                                                  "astc_recompression",
                                                                  Category::AdvancedGraphics};
    SwitchableSetting<bool> use_video_framerate{linkage, false, "use_video_framerate",
                                                Category::AdvancedGraphics};
    SwitchableSetting<bool> barrier_feedback_loops{linkage, true, "barrier_feedback_loops",
                                                   Category::AdvancedGraphics};

    SwitchableSetting<u8> bg_red{linkage, 0, "bg_red", Category::Renderer};
    SwitchableSetting<u8> bg_green{linkage, 0, "bg_green", Category::Renderer};
    SwitchableSetting<u8> bg_blue{linkage, 0, "bg_blue", Category::Renderer};

    // System
    SwitchableSetting<bool> rng_seed_enabled{linkage, false, "rng_seed_enabled", Category::System};
    SwitchableSetting<u32> rng_seed{linkage, 0, "rng_seed", Category::System};
    Setting<std::string> device_name{linkage, "Yuzu", "device_name", Category::System};
    // Measured in seconds since epoch
    Setting<bool> custom_rtc_enabled{linkage, false, "custom_rtc_enabled", Category::System};
    Setting<s64> custom_rtc{linkage, 0, "custom_rtc", Category::System};
    // Set on game boot, reset on stop. Seconds difference between current time and `custom_rtc`
    s64 custom_rtc_differential;

    Setting<s32> current_user{linkage, 0, "current_user", Category::System};
    SwitchableSetting<s32, true> language_index{linkage,         1, 0, 17, "language_index",
                                                Category::System};
    SwitchableSetting<s32, true> region_index{linkage, 1, 0, 6, "region_index", Category::System};
    SwitchableSetting<s32, true> time_zone_index{linkage,         0, 0, 45, "time_zone_index",
                                                 Category::System};
    SwitchableSetting<s32, true> sound_index{linkage, 1, 0, 2, "sound_index", Category::System};

    SwitchableSetting<bool> use_docked_mode{linkage, true, "use_docked_mode", Category::System};

    // Controls
    InputSetting<std::array<PlayerInput, 10>> players;

    Setting<bool, false,
// Only read/write enable_raw_input on Windows platforms
#ifdef _WIN32
            true
#else
            false
#endif
            >
        enable_raw_input{linkage, false, "enable_raw_input", Category::Controls};
    Setting<bool> controller_navigation{linkage, true, "controller_navigation", Category::Controls};
    Setting<bool> enable_joycon_driver{linkage, true, "enable_joycon_driver", Category::Controls};
    Setting<bool> enable_procon_driver{linkage, false, "enable_procon_driver", Category::Controls};

    SwitchableSetting<bool> vibration_enabled{linkage, true, "vibration_enabled",
                                              Category::Controls};
    SwitchableSetting<bool> enable_accurate_vibrations{linkage, false, "enable_accurate_vibrations",
                                                       Category::Controls};

    SwitchableSetting<bool> motion_enabled{linkage, true, "motion_enabled", Category::Controls};
    Setting<std::string> udp_input_servers{linkage, "127.0.0.1:26760", "udp_input_servers",
                                           Category::Controls};
    Setting<bool> enable_udp_controller{linkage, false, "enable_udp_controller",
                                        Category::Controls};

    Setting<bool> pause_tas_on_load{linkage, true, "pause_tas_on_load", Category::Controls};
    Setting<bool> tas_enable{linkage, false, "tas_enable", Category::Controls};
    Setting<bool> tas_loop{linkage, false, "tas_loop", Category::Controls};

    Setting<bool, false, false> mouse_panning{linkage, false, "mouse_panning", Category::Controls};
    Setting<u8, true> mouse_panning_sensitivity{
        linkage, 50, 1, 100, "mouse_panning_sensitivity", Category::Controls};
    Setting<bool> mouse_enabled{linkage, false, "mouse_enabled", Category::Controls};

    Setting<u8, true> mouse_panning_x_sensitivity{
        linkage, 50, 1, 100, "mouse_panning_x_sensitivity", Category::Controls};
    Setting<u8, true> mouse_panning_y_sensitivity{
        linkage, 50, 1, 100, "mouse_panning_y_sensitivity", Category::Controls};
    Setting<u8, true> mouse_panning_deadzone_counterweight{
        linkage, 20, 0, 100, "mouse_panning_deadzone_counterweight", Category::Controls};
    Setting<u8, true> mouse_panning_decay_strength{
        linkage, 18, 0, 100, "mouse_panning_decay_strength", Category::Controls};
    Setting<u8, true> mouse_panning_min_decay{
        linkage, 6, 0, 100, "mouse_panning_min_decay", Category::Controls};

    Setting<bool> emulate_analog_keyboard{linkage, false, "emulate_analog_keyboard",
                                          Category::Controls};
    Setting<bool> keyboard_enabled{linkage, false, "keyboard_enabled", Category::Controls};

    Setting<bool> debug_pad_enabled{linkage, false, "debug_pad_enabled", Category::Controls};
    ButtonsRaw debug_pad_buttons;
    AnalogsRaw debug_pad_analogs;

    TouchscreenInput touchscreen;

    Setting<std::string> touch_device{linkage, "min_x:100,min_y:50,max_x:1800,max_y:850",
                                      "touch_device", Category::Controls};
    Setting<int> touch_from_button_map_index{linkage, 0, "touch_from_button_map",
                                             Category::Controls};
    std::vector<TouchFromButtonMap> touch_from_button_maps;

    Setting<bool> enable_ring_controller{linkage, true, "enable_ring_controller",
                                         Category::Controls};
    RingconRaw ringcon_analogs;

    Setting<bool> enable_ir_sensor{linkage, false, "enable_ir_sensor", Category::Controls};
    Setting<std::string> ir_sensor_device{linkage, "auto", "ir_sensor_device", Category::Controls};

    Setting<bool> random_amiibo_id{linkage, false, "random_amiibo_id", Category::Controls};

    // Data Storage
    Setting<bool> use_virtual_sd{linkage, true, "use_virtual_sd", Category::DataStorage};
    Setting<bool> gamecard_inserted{linkage, false, "gamecard_inserted", Category::DataStorage};
    Setting<bool> gamecard_current_game{linkage, false, "gamecard_current_game",
                                        Category::DataStorage};
    Setting<std::string> gamecard_path{linkage, std::string(), "gamecard_path",
                                       Category::DataStorage};

    // Debugging
    bool record_frame_times;
    Setting<bool> use_gdbstub{linkage, false, "use_gdbstub", Category::Debugging};
    Setting<u16> gdbstub_port{linkage, 6543, "gdbstub_port", Category::Debugging};
    Setting<std::string> program_args{linkage, std::string(), "program_args", Category::Debugging};
    Setting<bool> dump_exefs{linkage, false, "dump_exefs", Category::Debugging};
    Setting<bool> dump_nso{linkage, false, "dump_nso", Category::Debugging};
    Setting<bool, false, false> dump_shaders{linkage, false, "dump_shaders", Category::Debugging};
    Setting<bool, false, false> dump_macros{linkage, false, "dump_macros", Category::Debugging};
    Setting<bool> enable_fs_access_log{linkage, false, "enable_fs_access_log", Category::Debugging};
    Setting<bool, false, false> reporting_services{linkage, false, "reporting_services",
                                                   Category::Debugging};
    Setting<bool> quest_flag{linkage, false, "quest_flag", Category::Debugging};
    Setting<bool> disable_macro_jit{linkage, false, "disable_macro_jit", Category::Debugging};
    Setting<bool> disable_macro_hle{linkage, false, "disable_macro_hle", Category::Debugging};
    Setting<bool, false, false> extended_logging{linkage, false, "extended_logging",
                                                 Category::Debugging};
    Setting<bool> use_debug_asserts{linkage, false, "use_debug_asserts", Category::Debugging};
    Setting<bool, false, false> use_auto_stub{linkage, false, "use_auto_stub", Category::Debugging};
    Setting<bool> enable_all_controllers{linkage, false, "enable_all_controllers",
                                         Category::Debugging};
    Setting<bool> create_crash_dumps{linkage, false, "create_crash_dumps", Category::Debugging};
    Setting<bool> perform_vulkan_check{linkage, true, "perform_vulkan_check", Category::Debugging};

    // Miscellaneous
    Setting<std::string> log_filter{linkage, "*:Info", "log_filter", Category::Miscellaneous};
    Setting<bool> use_dev_keys{linkage, false, "use_dev_keys", Category::Miscellaneous};

    // Network
    Setting<std::string> network_interface{linkage, std::string(), "network_interface",
                                           Category::Network};

    // WebService
    Setting<bool> enable_telemetry{linkage, true, "enable_telemetry", Category::WebService};
    Setting<std::string> web_api_url{linkage, "https://api.yuzu-emu.org", "web_api_url",
                                     Category::WebService};
    Setting<std::string> yuzu_username{linkage, std::string(), "yuzu_username",
                                       Category::WebService};
    Setting<std::string> yuzu_token{linkage, std::string(), "yuzu_token", Category::WebService};

    // Add-Ons
    std::map<u64, std::vector<std::string>> disabled_addons;
};

extern Values values;

bool IsConfiguringGlobal();
void SetConfiguringGlobal(bool is_global);

bool IsGPULevelExtreme();
bool IsGPULevelHigh();

bool IsFastmemEnabled();

float Volume();

std::string GetTimeZoneString();

void LogSettings();

void UpdateRescalingInfo();

// Restore the global state of all applicable settings in the Values struct
void RestoreGlobalState(bool is_powered_on);

} // namespace Settings
