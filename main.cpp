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
#include <hyprutils/string/ConstVarList.hpp>
#include <hyprland/src/desktop/view/Window.hpp>

#include "globals.hpp"

using namespace Hyprutils::String;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

struct SAppConfig {
    std::string szClass;
    float sat;
    int resX = -1;
    int resY = -1;
    float refreshRate = -1.0f;
};

std::vector<SAppConfig> g_appConfigs;
float g_globalSaturation = 0.0f; // 0 = disabled, >0 = apply globally

static const SAppConfig* getAppConfig(const std::string& appClass) {
    for (const auto& ac : g_appConfigs) {
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
const Mat3x3 calc_ctm_matrix(float sat) {
    std::array<float, 9> mat;
    float coeff = (1.0 - sat) / 3.0;
    for (int i = 0; i < 9; i++) {
        mat[i] = coeff + (i % 4 == 0 ? sat : 0);
    }
    return mat;
}

static std::string buildMonitorCommand(const std::string& name, int resX, int resY, float refreshRate, const Vector2D& offset, float scale) {
    return std::format("{},{}x{}@{},{}x{},{}", name, resX, resY, refreshRate, (int)offset.x, (int)offset.y, scale);
}

void applyGlobalSaturationToAllMonitors() {
    if (g_globalSaturation <= 0) return;
    
    // Log::logger->log(Log::ERR, "[hyprvibr] Applying global saturation {} to all monitors", g_globalSaturation);
    
    for (auto& mon : g_pCompositor->m_monitors) {
        if (mon) {
            mon->setCTM(calc_ctm_matrix(g_globalSaturation));
            // Log::logger->log(Log::ERR, "[hyprvibr] Applied global saturation to monitor {}", mon->m_name);
        }
    }
}

static PHLMONITOR pickMonitorForNoWindow(const PHLMONITOR& previous) {
    if (previous)
        return previous;

    if (auto focusedMon = g_pCompositor->getMonitorFromCursor(); focusedMon)
        return focusedMon;

    for (const auto& candidate : g_pCompositor->m_monitors) {
        if (candidate)
            return candidate;
    }

    return {};
}

void onActiveWindowChange(const PHLWINDOW win) {
    if (win) {
        // Log::logger->log(Log::ERR, "[hyprvibr] Window: {} ({})", win->m_title, win->m_initialClass);
    } else {
        // Log::logger->log(Log::ERR, "[hyprvibr] No window - DESKTOP/UNFOCUSED");
        // HyprlandAPI::addNotification(PHANDLE, "No window focused - applying global sat", 
        //                              CHyprColor{1.0, 1.0, 0.0, 1.0}, 2000);
    }
    
    // Log::logger->log(Log::ERR, "[hyprvibr] g_globalSaturation = {}", g_globalSaturation);
    
    const auto CONFIG = win ? getAppConfig(win->m_initialClass) : nullptr;
    auto prevMon = g_activeMonitor.lock();
    PHLMONITOR newMon;
    float newSat;
    int newResX = -1;
    int newResY = -1;

    if (!win) {
        // No window focused - use global saturation on current/previous monitor
        if (g_globalSaturation > 0) {
            newMon = pickMonitorForNoWindow(prevMon);
            if (newMon) {
                g_activeMonitor = newMon;
                // Log::logger->log(Log::ERR, "[hyprvibr] Branch: No window - selected monitor {}", newMon->m_name);
            } else {
                g_activeMonitor = {};
                // Log::logger->log(Log::ERR, "[hyprvibr] Branch: No window - NO MONITORS FOUND!");
            }
            newSat = g_globalSaturation;
            // Log::logger->log(Log::ERR, "[hyprvibr] Branch: No window - will apply global saturation {} to {}",
            //                g_globalSaturation, newMon ? newMon->m_name : "NO MONITOR");
        } else {
            // No global saturation, disable
            g_activeMonitor = {};
            newMon = {};
            newSat = 0;
            // Log::logger->log(Log::ERR, "[hyprvibr] Branch: No window - no global sat");
        }
    } else if (CONFIG == nullptr) {
        // No app-specific config - use global saturation if enabled
        if (g_globalSaturation > 0) {
            g_activeMonitor = win->m_monitor;
            newMon = win->m_monitor.lock();
            newSat = g_globalSaturation;
            // Log::logger->log(Log::ERR, "[hyprvibr] Branch: Using GLOBAL saturation {} for '{}'", g_globalSaturation, win->m_initialClass);
        } else {
            // Global saturation disabled, no effect
            g_activeMonitor = {};
            newMon = {};
            newSat = 0;
            // Log::logger->log(Log::ERR, "[hyprvibr] Branch: Global disabled (value={})", g_globalSaturation);
        }
    } else {
        // Use app-specific config (overrides global)
        g_activeMonitor = win->m_monitor;
        newMon = win->m_monitor.lock();
        newSat = CONFIG->sat;
        newResX = CONFIG->resX;
        newResY = CONFIG->resY;
        // Log::logger->log(Log::ERR, "[hyprvibr] Branch: Using APP-SPECIFIC config for '{}': sat={}", win->m_initialClass, newSat);
    }

    bool settingsChanged = prevMon != newMon || newSat != g_activeMonitorSat || newResX != g_activeResX || newResY != g_activeResY;
    
    // Log::logger->log(Log::ERR, "[hyprvibr] settingsChanged={}, newSat={}, prevSat={}, hasMonitor={}",
    //                  settingsChanged, newSat, g_activeMonitorSat, (bool)newMon);

    // Always apply CTM if we have a monitor (to prevent resets from other sources)
    if (newMon) {
        // Log::logger->log(Log::ERR, "[hyprvibr] About to apply CTM: monitor={}, sat={}", newMon->m_name, newSat);
        if (newSat > 0) {
            newMon->setCTM(calc_ctm_matrix(newSat));
            // Log::logger->log(Log::ERR, "[hyprvibr] ✓✓✓ APPLIED saturation {} to {}", newSat, newMon->m_name);
        } else {
            newMon->setCTM(Mat3x3::identity());
            // Log::logger->log(Log::ERR, "[hyprvibr] Reset saturation to identity on {}", newMon->m_name);
        }
    } else {
        // Log::logger->log(Log::ERR, "[hyprvibr] SKIPPED CTM application - no monitor!");
    }

    if (settingsChanged) {
        if (prevMon && prevMon != newMon) {
            prevMon->setCTM(Mat3x3::identity());

            if (g_originalMonitorRule.has_value()) {
                auto cmd = buildMonitorCommand(prevMon->m_name, (int)g_originalMonitorRule->resolution.x, (int)g_originalMonitorRule->resolution.y,
                                                g_originalMonitorRule->refreshRate, g_originalMonitorRule->offset, g_originalMonitorRule->scale);
                HyprlandAPI::invokeHyprctlCommand("keyword", "monitor " + cmd);
                // Log::logger->log(Log::ERR, "[hyprvibr] Restored monitor {}", prevMon->m_name);
                g_originalMonitorRule.reset();
            }
        }

        // Handle resolution changes for app-specific configs
        if (newMon) {
            if (CONFIG && CONFIG->resX > 0 && CONFIG->resY > 0) {
                auto currentResX = (int)newMon->m_pixelSize.x;
                auto currentResY = (int)newMon->m_pixelSize.y;

                if (!g_originalMonitorRule.has_value()) {
                    g_originalMonitorRule = newMon->m_activeMonitorRule;
                }

                if (currentResX != CONFIG->resX || currentResY != CONFIG->resY) {
                    float refreshRate = CONFIG->refreshRate > 0 ? CONFIG->refreshRate : 60.0f;
                    auto cmd = buildMonitorCommand(newMon->m_name, CONFIG->resX, CONFIG->resY, refreshRate,
                                                    newMon->m_activeMonitorRule.offset, newMon->m_activeMonitorRule.scale);
                    HyprlandAPI::invokeHyprctlCommand("keyword", "monitor " + cmd);
                    // Log::logger->log(Log::INFO, "[hyprvibr] Changed resolution to {}x{}@{} on {}", CONFIG->resX, CONFIG->resY, refreshRate, newMon->m_name);
                }
            } else if (g_activeResX > 0 && g_activeResY > 0 && g_originalMonitorRule.has_value()) {
                auto cmd = buildMonitorCommand(newMon->m_name, (int)g_originalMonitorRule->resolution.x, (int)g_originalMonitorRule->resolution.y,
                                                g_originalMonitorRule->refreshRate, g_originalMonitorRule->offset, g_originalMonitorRule->scale);
                HyprlandAPI::invokeHyprctlCommand("keyword", "monitor " + cmd);
                // Log::logger->log(Log::INFO, "[hyprvibr] Restored monitor {}", newMon->m_name);
                g_originalMonitorRule.reset();
            }
        }

        g_activeMonitorSat = newSat;
        g_activeResX = newResX;
        g_activeResY = newResY;
    }
    
    // Log::logger->log(Log::ERR, "[hyprvibr] === END (final sat={}) ===", g_activeMonitorSat);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        // HyprlandAPI::addNotification(PHANDLE, "[hyprvibr] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
        //                              CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprvibr] Version mismatch");
    }

    static auto P = HyprlandAPI::registerCallbackDynamic(PHANDLE, "activeWindow", [&](void* self, SCallbackInfo& info, std::any data) {
        const auto WIN = std::any_cast<PHLWINDOW>(data);
        onActiveWindowChange(WIN);
    });

    static auto P2 = HyprlandAPI::registerCallbackDynamic(PHANDLE, "preConfigReload", [&](void* self, SCallbackInfo& info, std::any data) {
        g_appConfigs.clear();
        g_globalSaturation = 0.0f;
        // HyprlandAPI::addNotification(PHANDLE, "Config reload started", CHyprColor{1.0, 1.0, 0.0, 1.0}, 10000);
        // Log::logger->log(Log::ERR, "[hyprvibr] ===== CONFIG RELOAD - CLEARING DATA =====");
    });

    static auto P3 = HyprlandAPI::registerCallbackDynamic(PHANDLE, "configReloaded", [&](void* self, SCallbackInfo& info, std::any data) {
        // HyprlandAPI::addNotification(PHANDLE,
        //     std::format("Config reloaded - global sat: {}", g_globalSaturation).c_str(),
        //     CHyprColor{0.0, 1.0, 1.0, 1.0}, 10000);
        // Log::logger->log(Log::ERR, "[hyprvibr] ===== CONFIG RELOADED - global sat is {} =====", g_globalSaturation);
        
        // Apply global saturation to all monitors immediately if set
        applyGlobalSaturationToAllMonitors();
        
        onActiveWindowChange(Desktop::focusState()->window());
    });

    // Global saturation (applies to all windows without specific config)
    HyprlandAPI::addConfigKeyword(
        PHANDLE, "hyprvibr-saturation",
        [](const char* l, const char* r) -> Hyprlang::CParseResult {
            Hyprlang::CParseResult result;
            try {
                g_globalSaturation = std::stof(r);
                // Log::logger->log(Log::ERR, "[hyprvibr] ===== GLOBAL SATURATION SET TO {} =====", g_globalSaturation);
                // HyprlandAPI::addNotification(PHANDLE,
                //     std::format("Global saturation: {}", g_globalSaturation).c_str(),
                //     CHyprColor{1.0, 0.5, 0.0, 1.0}, 10000);
            } catch (std::exception& e) {
                result.setError("failed to parse saturation value");
                return result;
            }
            return result;
        },
        Hyprlang::SHandlerOptions{});

    // Per-app configuration
    HyprlandAPI::addConfigKeyword(
        PHANDLE, "hyprvibr-app",
        [](const char* l, const char* r) -> Hyprlang::CParseResult {
            const std::string      str = r;
            CConstVarList          data(str, 0, ',', true);

            Hyprlang::CParseResult result;

            if (data.size() < 2 || data.size() > 5) {
                result.setError("hyprvibr-app requires 2-5 params: class,sat[,resX,resY[,refreshRate]]");
                return result;
            }

            try {
                SAppConfig config;
                config.szClass = data[0];
                config.sat = std::stof(std::string{data[1]});

                if (data.size() >= 4) {
                    config.resX = std::stoi(std::string{data[2]});
                    config.resY = std::stoi(std::string{data[3]});
                }

                if (data.size() >= 5) {
                    config.refreshRate = std::stof(std::string{data[4]});
                }

                g_appConfigs.emplace_back(std::move(config));
            } catch (std::exception& e) {
                result.setError("failed to parse line");
                return result;
            }

            return result;
        },
        Hyprlang::SHandlerOptions{});

    // HyprlandAPI::addNotification(PHANDLE, "Hyprvibr loaded - ready!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 3000);
    // Log::logger->log(Log::ERR, "[hyprvibr] ===== PLUGIN LOADED =====");
    return {"hyprvibr", "A plugin to customize monitor saturation per focused window", "devcexx", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    onActiveWindowChange({});
}
