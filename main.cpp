#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
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
float g_globalSaturation = 0.0f; // 0 = disabled, >0 = apply globally

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
std::optional<SMonitorRule> g_originalMonitorRule;

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
    return std::format("{},{}x{}@{},{}x{},{}", name, resX, resY, refreshRate, (int)offset.x, (int)offset.y, scale);
}

void applyGlobalSaturationToAllMonitors()
{
    if (g_globalSaturation <= 0)
        return;

    for (auto &mon : g_pCompositor->m_monitors)
    {
        if (mon)
            mon->setCTM(calc_ctm_matrix(g_globalSaturation));
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
    const auto CONFIG = win ? getAppConfig(win->m_initialClass) : nullptr;
    auto prevMon = g_activeMonitor.lock();
    PHLMONITOR newMon;
    float newSat;
    int newResX = -1;
    int newResY = -1;

    if (!win)
    {
        if (g_globalSaturation > 0)
        {
            newMon = pickMonitorForNoWindow(prevMon);
            if (newMon)
                g_activeMonitor = newMon;
            else
                g_activeMonitor = {};
            newSat = g_globalSaturation;
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
        if (g_globalSaturation > 0)
        {
            g_activeMonitor = win->m_monitor;
            newMon = win->m_monitor.lock();
            newSat = g_globalSaturation;
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
                auto cmd = buildMonitorCommand(prevMon->m_name, (int)g_originalMonitorRule->resolution.x, (int)g_originalMonitorRule->resolution.y,
                                               g_originalMonitorRule->refreshRate, g_originalMonitorRule->offset, g_originalMonitorRule->scale);
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
                                                   newMon->m_activeMonitorRule.offset, newMon->m_activeMonitorRule.scale);
                    HyprlandAPI::invokeHyprctlCommand("keyword", "monitor " + cmd);
                }
            }
            else if (g_activeResX > 0 && g_activeResY > 0 && g_originalMonitorRule.has_value())
            {
                auto cmd = buildMonitorCommand(newMon->m_name, (int)g_originalMonitorRule->resolution.x, (int)g_originalMonitorRule->resolution.y,
                                               g_originalMonitorRule->refreshRate, g_originalMonitorRule->offset, g_originalMonitorRule->scale);
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

    if (HASH != CLIENT_HASH)
    {
        HyprlandAPI::addNotification(PHANDLE,
                                     "[hyprvibr] Hyprland API hash mismatch; attempting best-effort compatibility.",
                                     CHyprColor{1.0, 0.6, 0.0, 1.0}, 6000);
    }

    static auto P = HyprlandAPI::registerCallbackDynamic(PHANDLE, "activeWindow", [&](void *self, SCallbackInfo &info, std::any data)
                                                         {
        const auto WIN = windowFromCallbackPayload(data);
        onActiveWindowChange(WIN); });

    static auto P2 = HyprlandAPI::registerCallbackDynamic(PHANDLE, "preConfigReload", [&](void *self, SCallbackInfo &info, std::any data)
                                                          {
        g_appConfigs.clear();
        g_globalSaturation = 0.0f; });

    static auto P3 = HyprlandAPI::registerCallbackDynamic(PHANDLE, "configReloaded", [&](void *self, SCallbackInfo &info, std::any data)
                                                          {
        applyGlobalSaturationToAllMonitors();
        onActiveWindowChange(Desktop::focusState()->window()); });

    HyprlandAPI::addConfigKeyword(
        PHANDLE, "hyprvibr-saturation",
        [](const char *l, const char *r) -> Hyprlang::CParseResult
        {
            Hyprlang::CParseResult result;
            try
            {
                g_globalSaturation = std::stof(r);
            }
            catch (std::exception &e)
            {
                result.setError("failed to parse saturation value");
                return result;
            }
            return result;
        },
        Hyprlang::SHandlerOptions{});

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

    return {"hyprvibr", "A plugin to customize monitor saturation per focused window", "devcexx", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT()
{
    onActiveWindowChange({});
}
