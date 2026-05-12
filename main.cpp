#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <any>
#include <array>
#include <format>
#include <memory>
#include <hyprutils/string/ConstVarList.hpp>
#include <hyprland/src/desktop/view/Window.hpp>

#include "globals.hpp"

using namespace Hyprutils::String;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION()
{
    return HYPRLAND_API_VERSION;
}

struct SAppConfig
{
    std::string szClass;
    float sat;
    int resX = -1;
    int resY = -1;
    float refreshRate = -1.0f;
};

std::vector<SAppConfig> g_appConfigs;

SP<Config::Values::CFloatValue> g_pGlobalSaturation;

// Returns the current global saturation from the typed config value.
// Falls back to 0 (disabled) if the value hasn't been registered yet.
static float getGlobalSaturation()
{
    if (g_pGlobalSaturation)
        return g_pGlobalSaturation->value();
    return 0.0f;
}

static const SAppConfig *getAppConfig(const std::string &appClass)
{
    for (const auto &ac : g_appConfigs)
    {
        if (ac.szClass != appClass)
            continue;
        return &ac;
    }
    return nullptr;
}

PHLMONITORREF g_activeMonitor;
float g_activeMonitorSat;
int g_activeResX = -1;
int g_activeResY = -1;
std::optional<Config::CMonitorRule> g_originalMonitorRule;

// Evily stoled from libvibrant
const Mat3x3 calc_ctm_matrix(float sat)
{
    std::array<float, 9> mat;
    float coeff = (1.0 - sat) / 3.0;
    for (int i = 0; i < 9; i++)
    {
        mat[i] = coeff + (i % 4 == 0 ? sat : 0);
    }
    return mat;
}

static std::string buildMonitorCommand(const std::string &name, int resX, int resY, float refreshRate, const Vector2D &offset, float scale)
{
    return name + "," + std::to_string(resX) + "x" + std::to_string(resY) + "@" + std::to_string(refreshRate) + "," + std::to_string((int)offset.x) + "x" + std::to_string((int)offset.y) + "," + std::to_string(scale);
}

void applyGlobalSaturationToAllMonitors()
{
    const float globalSat = getGlobalSaturation();
    if (globalSat <= 0)
        return;

    for (auto &mon : g_pCompositor->m_monitors)
    {
        if (mon)
            mon->setCTM(calc_ctm_matrix(globalSat));
    }
}

static PHLMONITOR pickMonitorForNoWindow(const PHLMONITOR &previous)
{
    if (previous)
        return previous;

    if (auto focusedMon = g_pCompositor->getMonitorFromCursor(); focusedMon)
        return focusedMon;

    for (const auto &candidate : g_pCompositor->m_monitors)
    {
        if (candidate)
            return candidate;
    }

    return {};
}

static PHLWINDOW windowFromCallbackPayload(const std::any &payload)
{
    if (!payload.has_value())
        return {};

    if (payload.type() == typeid(PHLWINDOW))
    {
        try
        {
            return std::any_cast<PHLWINDOW>(payload);
        }
        catch (const std::bad_any_cast &)
        {
            return {};
        }
    }

    if (payload.type() == typeid(PHLWINDOWREF))
    {
        try
        {
            const auto ref = std::any_cast<PHLWINDOWREF>(payload);
            return ref.lock();
        }
        catch (const std::bad_any_cast &)
        {
            return {};
        }
    }

    return {};
}

void onActiveWindowChange(const PHLWINDOW win)
{
    const float globalSat = getGlobalSaturation();
    const auto CONFIG = win ? getAppConfig(win->m_initialClass) : nullptr;
    auto prevMon = g_activeMonitor.lock();
    PHLMONITOR newMon;
    float newSat;
    int newResX = -1;
    int newResY = -1;

    if (!win)
    {
        if (globalSat > 0)
        {
            newMon = pickMonitorForNoWindow(prevMon);
            if (newMon)
                g_activeMonitor = newMon;
            else
                g_activeMonitor = {};
            newSat = globalSat;
        }
        else
        {
            g_activeMonitor = {};
            newMon = {};
            newSat = 0;
        }
    }
    else if (CONFIG == nullptr)
    {
        if (globalSat > 0)
        {
            g_activeMonitor = win->m_monitor;
            newMon = win->m_monitor.lock();
            newSat = globalSat;
        }
        else
        {
            g_activeMonitor = {};
            newMon = {};
            newSat = 0;
        }
    }
    else
    {
        g_activeMonitor = win->m_monitor;
        newMon = win->m_monitor.lock();
        newSat = CONFIG->sat;
        newResX = CONFIG->resX;
        newResY = CONFIG->resY;
    }

    bool settingsChanged = prevMon != newMon || newSat != g_activeMonitorSat || newResX != g_activeResX || newResY != g_activeResY;

    if (newMon)
    {
        if (newSat > 0)
            newMon->setCTM(calc_ctm_matrix(newSat));
        else
            newMon->setCTM(Mat3x3::identity());
    }

    if (settingsChanged)
    {
        if (prevMon && prevMon != newMon)
        {
            prevMon->setCTM(Mat3x3::identity());

            if (g_originalMonitorRule.has_value())
            {
                auto cmd = buildMonitorCommand(prevMon->m_name, (int)g_originalMonitorRule->m_resolution.x, (int)g_originalMonitorRule->m_resolution.y,
                                               g_originalMonitorRule->m_refreshRate, g_originalMonitorRule->m_offset, g_originalMonitorRule->m_scale);
                HyprlandAPI::invokeHyprctlCommand("keyword", "monitor " + cmd);
                g_originalMonitorRule.reset();
            }
        }

        if (newMon)
        {
            if (CONFIG && CONFIG->resX > 0 && CONFIG->resY > 0)
            {
                auto currentResX = (int)newMon->m_pixelSize.x;
                auto currentResY = (int)newMon->m_pixelSize.y;

                if (!g_originalMonitorRule.has_value())
                    g_originalMonitorRule = newMon->m_activeMonitorRule;

                if (currentResX != CONFIG->resX || currentResY != CONFIG->resY)
                {
                    float refreshRate = CONFIG->refreshRate > 0 ? CONFIG->refreshRate : 60.0f;
                    auto cmd = buildMonitorCommand(newMon->m_name, CONFIG->resX, CONFIG->resY, refreshRate,
                                                   newMon->m_activeMonitorRule.m_offset, newMon->m_activeMonitorRule.m_scale);
                    HyprlandAPI::invokeHyprctlCommand("keyword", "monitor " + cmd);
                }
            }
            else if (g_activeResX > 0 && g_activeResY > 0 && g_originalMonitorRule.has_value())
            {
                auto cmd = buildMonitorCommand(newMon->m_name, (int)g_originalMonitorRule->m_resolution.x, (int)g_originalMonitorRule->m_resolution.y,
                                               g_originalMonitorRule->m_refreshRate, g_originalMonitorRule->m_offset, g_originalMonitorRule->m_scale);
                HyprlandAPI::invokeHyprctlCommand("keyword", "monitor " + cmd);
                g_originalMonitorRule.reset();
            }
        }

        g_activeMonitorSat = newSat;
        g_activeResX = newResX;
        g_activeResY = newResY;
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle)
{
    PHANDLE = handle;

    const std::string HASH = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    // --- Typed config value (non-deprecated) for global saturation ---
    g_pGlobalSaturation = Hyprutils::Memory::makeShared<Config::Values::CFloatValue>("plugin:hyprvibr:saturation", "Global saturation", 0.0f);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pGlobalSaturation);

    // --- Per-app config keyword ---
    // addConfigKeyword is the only API that supports repeated/list-style keys.
    // The deprecation warning is suppressed here because no typed replacement
    // exists for this use-case. Track https://github.com/hyprwm/Hyprland for
    // a future addConfigKeyword successor and remove the pragma when available.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    HyprlandAPI::addConfigKeyword(
        PHANDLE, "hyprvibr-app",
        [](const char *l, const char *r) -> Hyprlang::CParseResult
        {
            const std::string str = r;
            CConstVarList data(str, 0, ',', true);

            Hyprlang::CParseResult result;

            if (data.size() < 2 || data.size() > 5)
            {
                result.setError("hyprvibr-app requires 2-5 params: class,sat[,resX,resY[,refreshRate]]");
                return result;
            }

            try
            {
                SAppConfig config;
                config.szClass = data[0];
                config.sat = std::stof(std::string{data[1]});

                if (data.size() >= 4)
                {
                    config.resX = std::stoi(std::string{data[2]});
                    config.resY = std::stoi(std::string{data[3]});
                }

                if (data.size() >= 5)
                    config.refreshRate = std::stof(std::string{data[4]});

                g_appConfigs.emplace_back(std::move(config));
            }
            catch (std::exception &e)
            {
                result.setError("failed to parse line");
                return result;
            }

            return result;
        },
        Hyprlang::SHandlerOptions{});
#pragma GCC diagnostic pop

    // v0.54.0: registerCallbackDynamic is gone, use Event::bus() typed listeners.
    // CHyprSignalListener MUST be kept alive — it auto-unregisters on destruction.
    static auto P = Event::bus()->m_events.window.active.listen([](PHLWINDOW win, Desktop::eFocusReason reason)
                                                                { onActiveWindowChange(win); });

    static auto P2 = Event::bus()->m_events.config.preReload.listen([]()
                                                                    { g_appConfigs.clear(); });

    static auto P3 = Event::bus()->m_events.config.reloaded.listen([]()
                                                                   {
        applyGlobalSaturationToAllMonitors();
        onActiveWindowChange(Desktop::focusState()->window()); });

    return {"hyprvibr", "A plugin to customize monitor saturation per focused window", "devcexx", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT()
{
    onActiveWindowChange({});
}
