# PostProcess Controller System

## Overview

The PostProcess Controller provides a centralized, priority-based mechanism for external sources to override PostProcessFeature parameters at runtime. It solves the problem of multiple systems (Weather, other Features, external mods) needing to control the same post-processing settings without conflicting.

Key properties:

-   **Discoverable**: All controllable parameters are registered with type, name, range, and documentation. External callers can enumerate them at runtime.
-   **Priority-based**: When multiple sources override the same parameter, the highest priority wins. No blending.
-   **Non-destructive**: Original settings are never lost. Overrides are applied before `Draw()` and reverted immediately after. Removing all overrides restores the user's settings.

## Architecture

### Core Components

**PostProcessController.h** contains:

-   **`PPOverrideValue`**: A `std::variant<bool, int, float, float2, float3, float4>` representing any overridable value.
-   **`PPParamDesc`**: Descriptor for a registered parameter, including its type, pointer to the setting field, display name, tooltip, default value, and optional min/max range.
-   **`PostProcessController`**: Singleton that manages the parameter registry and override stacks.

### Data Flow

```
Feature Initialization → Parameter Registry → Override Management → Apply/Revert Cycle
         ↓                       ↓                    ↓                    ↓
 RegisterControllableParams   PPParamDesc         SetOverride()      ApplyOverrides()
         ↓                 (type, ptr, name,     RemoveOverride()    Draw()
  Each feature declares     range, default)     RemoveAllFromSource  RevertOverrides()
  which settings are                                  ↓
  externally controllable                       OverrideStack
                                             (sorted by priority)
```

### Lifecycle

1. **Registration** (`PostProcessing::SetupResources`): After constructing the pipeline, each `PostProcessFeature::RegisterControllableParams()` is called. Features register their controllable settings as `PPParamDesc` objects with the singleton controller.

2. **Override setting** (any time): External sources call `SetOverride()` to push an override onto a parameter's stack. Multiple sources can override the same parameter; the highest priority wins.

3. **Apply/Revert** (each frame, per feature): In the Draw loops, the controller calls `ApplyOverrides(featureType)` before `Draw()` to write winning override values into the settings fields, then `RevertOverrides(featureType)` after `Draw()` to restore the originals.

## Usage Guide

### Setting Overrides (for control sources)

Any system that wants to control post-processing parameters interacts with `PostProcessController`:

```cpp
auto* ctrl = PostProcessController::GetSingleton();

// Weather system adjusts bloom threshold
ctrl->SetOverride("COD Bloom", "Threshold", "WeatherManager",
    PostProcessController::kWeather, -4.0f);

// Another feature wants to override the same parameter with higher priority
ctrl->SetOverride("COD Bloom", "Threshold", "IBL",
    PostProcessController::kFeature, -2.0f);
// IBL wins (kFeature=200 > kWeather=100)

// Remove a single override
ctrl->RemoveOverride("COD Bloom", "Threshold", "IBL");
// Now WeatherManager's value takes effect again

// Remove all overrides from a source at once
ctrl->RemoveAllFromSource("WeatherManager");
```

### Enumerating Available Parameters

External systems can discover what parameters are available:

```cpp
auto* ctrl = PostProcessController::GetSingleton();

// List all controllable parameters for a specific feature
for (auto* desc : ctrl->GetRegisteredParams("COD Bloom")) {
    logger::info("{} ({}) - {} [type={}]",
        desc->name, desc->displayName, desc->tooltip,
        static_cast<int>(desc->type));
}

// List all controllable parameters across all features
for (auto* desc : ctrl->GetAllRegisteredParams()) {
    logger::info("[{}] {} ({})",
        desc->featureType, desc->name, desc->displayName);
}

// Find a specific parameter
const PPParamDesc* desc = ctrl->FindParam("COD Bloom", "Threshold");
if (desc) {
    // desc->type, desc->minValue, desc->maxValue, desc->defaultValue, etc.
}
```

### Query Active Overrides

```cpp
auto* ctrl = PostProcessController::GetSingleton();

// Check if any overrides are active for a feature
if (ctrl->HasOverridesForFeature("COD Bloom")) {
    // ...
}

// Get the winning value for a specific parameter
if (auto val = ctrl->GetActiveOverride("COD Bloom", "Threshold")) {
    float threshold = std::get<float>(*val);
}
```

### Priority Levels

The controller defines predefined priority constants. Custom integer values are also supported.

| Constant    | Value | Use Case                         |
| ----------- | ----- | -------------------------------- |
| `kWeather`  | 100   | Weather/Time-of-Day system       |
| `kFeature`  | 200   | Other Community Shaders features |
| `kExternal` | 300   | External mods/APIs               |
| `kDebug`    | 1000  | Debug overrides (always win)     |

Higher value = higher priority. When multiple sources override the same parameter, only the highest priority value is applied.

## Adding Controllable Parameters to a PostProcessFeature

### Step 1: Override RegisterControllableParams()

In your feature's `.h` file, declare the override:

```cpp
struct MyPPFeature : public PostProcessFeature
{
    // ... existing members ...

    virtual void RegisterControllableParams() override;
};
```

### Step 2: Register Parameters in the .cpp

```cpp
void MyPPFeature::RegisterControllableParams()
{
    auto* ctrl = PostProcessController::GetSingleton();
    const std::string ft = GetType();  // e.g. "My PP Feature"
    using T = PPParamDesc::Type;

    // Float parameter with min/max range
    ctrl->RegisterParam({
        ft,                         // featureType
        "intensity",                // name (internal key)
        "Effect Intensity",         // displayName (for UI)
        "Overall effect strength",  // tooltip
        T::Float,                   // type
        &settings.intensity,        // pointer to the setting field
        1.0f,                       // default value
        0.0f,                       // min (optional)
        2.0f                        // max (optional)
    });

    // Float4 parameter (no min/max for vector types)
    ctrl->RegisterParam({
        ft, "tintColor", "Tint Color", "RGBA tint applied to the effect",
        T::Float4, &settings.tintColor, float4{ 1.f, 1.f, 1.f, 1.f }
    });

    // Bool parameter
    ctrl->RegisterParam({
        ft, "enableBlur", "Enable Blur", "Toggle blur pass",
        T::Bool, &settings.enableBlur, true
    });
}
```

### Step 3: Done

No other changes needed. The controller automatically handles apply/revert in the Draw loop. The `PostProcessing.cpp` integration wraps every `pipe->Draw()` call with:

```cpp
ppController->ApplyOverrides(pipe->GetType());
pipe->Draw(lastTexColor);
ppController->RevertOverrides(pipe->GetType());
```

## Registered Parameter Reference

### Color Grading and Tone Mapping

| Name                        | Display Name                     | Type   | Default        | Description                                      |
| --------------------------- | -------------------------------- | ------ | -------------- | ------------------------------------------------ |
| `slope`                     | Slope                            | Float4 | (1,1,1,0)      | ASC CDL slope                                    |
| `power`                     | Power                            | Float4 | (1,1,1,0)      | ASC CDL power                                    |
| `cdlOffset`                 | CDL Offset                       | Float4 | (0,0,0,0)      | ASC CDL offset                                   |
| `lift`                      | Lift                             | Float4 | (0,0,0,0)      | Lift adjustment                                  |
| `gamma`                     | Gamma                            | Float4 | (0,0,0,0)      | Gamma adjustment                                 |
| `gain`                      | Gain                             | Float4 | (1,1,1,1)      | Gain adjustment                                  |
| `inOutGamma`                | Input/Output Gamma               | Float4 | (1,1,1,1)      | Input and output gamma values                    |
| `oklchSaturation`           | OKLCH Saturation                 | Float4 | (1,1,0,0)      | Saturation, vibrance, hue shift                  |
| `oklchColorMixer[0]`..`[6]` | OKLCH Color Mixer - Red..Magenta | Float4 | (0,1,0,0)      | Per-hue (hue shift, vibrance, brightness)        |
| `contrast`                  | Contrast                         | Float4 | (1,1,1,0)      | Contrast adjustment                              |
| `pivot`                     | Pivot                            | Float4 | (0.18,...)     | Contrast pivot point                             |
| `exposureTemperatureTint`   | Exposure/Temperature/Tint        | Float4 | (1,65,0,0)     | Exposure, color temperature, tint                |
| `shadowsGain`               | Shadows Gain                     | Float4 | (1,1,1,0)      | Shadows gain multiplier                          |
| `midtonesGain`              | Midtones Gain                    | Float4 | (1,1,1,0)      | Midtones gain multiplier                         |
| `highlightsGain`            | Highlights Gain                  | Float4 | (1,1,1,0)      | Highlights gain multiplier                       |
| `shadowsHighlightsRange`    | Shadows/Highlights Range         | Float4 | (0,0.3,0.55,1) | Shadow and highlight boundary ranges             |
| `shadowsOffset`             | Shadows Offset                   | Float4 | (0,0,0,0)      | Shadows color offset                             |
| `midtonesOffset`            | Midtones Offset                  | Float4 | (0,0,0,0)      | Midtones color offset                            |
| `highlightsOffset`          | Highlights Offset                | Float4 | (0,0,0,0)      | Highlights color offset                          |
| `gameCinematicBlend`        | Game Cinematic Blend             | Float3 | (1,1,1)        | Blend for game cinematic (sat, bright, contrast) |
| `gameFadeBlend`             | Game Fade Blend                  | Float  | 1.0            | Blend factor for game fade [0..1]                |
| `gameTintBlend`             | Game Tint Blend                  | Float  | 1.0            | Blend factor for game tint [0..1]                |

### COD Bloom

| Name             | Display Name    | Type  | Default | Range    | Description                      |
| ---------------- | --------------- | ----- | ------- | -------- | -------------------------------- |
| `Threshold`      | Bloom Threshold | Float | -6.0    | [-6, 21] | Luminance threshold in EV        |
| `UpsampleRadius` | Upsample Radius | Float | 2.0     | [1, 5]   | Upsampling blur radius (px)      |
| `BlendFactor`    | Bloom Mix       | Float | 0.05    | [0, 1]   | Blend between original and bloom |

### Vignette

| Name          | Display Name       | Type  | Default | Range    | Description                         |
| ------------- | ------------------ | ----- | ------- | -------- | ----------------------------------- |
| `FocalLength` | Focal Length       | Float | 1.0     | [0.1, 2] | Lens focal length relative to width |
| `Anamorphism` | Anamorphic Squeeze | Float | 1.0     | [0.1, 1] | Anamorphic lens simulation          |
| `Power`       | Power              | Float | 3.0     | [0, 4]   | Vignette power law exponent         |

### Histogram Auto Exposure

| Name                   | Display Name          | Type  | Default | Range     | Description               |
| ---------------------- | --------------------- | ----- | ------- | --------- | ------------------------- |
| `ExposureCompensation` | Exposure Compensation | Float | 0.0     | [-5, 5]   | Additional exposure in EV |
| `AdaptSpeed`           | Adaptation Speed      | Float | 1.5     | [0.1, 5]  | Eye adaptation speed      |
| `PurkinjeStartEV`      | Purkinje Start EV     | Float | -1.5    | [-10, 21] | EV where Purkinje begins  |
| `PurkinjeMaxEV`        | Purkinje Max EV       | Float | -4.0    | [-10, 21] | EV where Purkinje is max  |
| `PurkinjeStrength`     | Purkinje Strength     | Float | 0.0     | [0, 5]    | Max Purkinje blue shift   |

## Design Notes

### Thread Safety

All public methods on `PostProcessController` are thread-safe via `std::shared_mutex`. Read operations (`GetRegisteredParams`, `GetActiveOverride`, `HasOverridesForFeature`) use shared locks. Write operations (`SetOverride`, `RemoveOverride`, `ApplyOverrides`, `RevertOverrides`) use exclusive locks.

### Memory Model

-   `PPParamDesc::valuePtr` stores a raw pointer to the setting field inside each `PostProcessFeature`. This is safe because features outlive the controller (both are singletons / owned by `PostProcessing`).
-   `ApplyOverrides()` saves original values before writing, and `RevertOverrides()` restores them. This means the user's configured settings are never permanently modified.

### Performance

-   Registration is one-time during initialization.
-   `ApplyOverrides` / `RevertOverrides` only iterate over active override stacks for the given feature; if no overrides exist, the cost is a single map lookup.
-   Override stacks are kept sorted by priority (descending), so finding the winning value is O(1).

### Relationship to Weather Variable System

The PostProcess Controller is **independent** of the existing `WeatherVariableRegistry`. The Weather system is simply one possible control source that calls `SetOverride()` with `kWeather` priority. The two systems have different scopes:

-   **WeatherVariableRegistry**: Manages per-weather JSON serialization, interpolation during weather transitions, and UI integration for features that inherit from `Feature`.
-   **PostProcessController**: Manages runtime parameter overrides with priority conflict resolution for `PostProcessFeature` subclasses, which do not inherit from `Feature`.
