// VR\config.txt: completed from VR\config.default.txt at the start, read then and whenever it
// changes.
#include "vr.h"
#include <Windows.h>
#include "game.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <string>
#include <filesystem>
#include <type_traits>
#include <algorithm>
#include <vector>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iterator>

// [CONFIG PARSING UTILITY FUNCTION]
// Generates an error message by stringifying and concatenating 'args...'.
template <typename... Ts>
static void concatErrorMsg(Game& game, const Ts&... args)
{
    std::ostringstream oss;
    (oss << ... << args);
    game.errorMsg(oss.str().c_str());
}

// [CONFIG PARSING UTILITY FUNCTION]
// Attempts to parse an entry with key 'key' from the provided 'userConfig'. If the key is
// missing, 'defaultValue' is returned and logged: every update of the mod with an older config
// would ask otherwise, on the desktop, unseen in the headset. If the parsing fails, 'defaultValue'
// is returned and an error message is generated.
template <typename T>
static T parseConfigEntry(
    const std::unordered_map<std::string, std::string>& userConfig, Game& game,
    const char* key, const T& defaultValue)
try
{
    const auto itr = userConfig.find(key);

    if (itr == userConfig.end())
    {
        std::cout << "Config entry '" << key << "' missing, using the default\n";
        return defaultValue;
    }

    const std::string& configValue = itr->second;

    if constexpr (std::is_same_v<T, bool>)
    {
        return !_stricmp(configValue.c_str(), "true") || configValue == "1";
    }
    else if constexpr(std::is_floating_point_v<T>)
    {
        return std::stof(configValue);
    }
    else if constexpr(std::is_integral_v<T>)
    {
        return std::stol(configValue);
    }
    else
    {
        // Just a way of generating a compilation failure in case this branch is taken.
        struct invalid_type;
        return invalid_type{};
    }
}
catch (const std::logic_error& e)
{
    concatErrorMsg(game, "Error parsing config entry with key '", key,
        "' -- reverting to default value of '", defaultValue, "' -- error: (", e.what(), ")");

    throw;
}

// Updates keep the player's VR\config.txt: the mod ships VR\config.default.txt, makes config.txt
// from it if it's missing, and adds the keys that are new in it, with their comments. Values
// still at a former default get the current default line, once: the first line names the mod
// version that wrote the file, and a migration only runs for files older than it.
void VR::CompleteConfigFile()
{
    using Version = std::array<int, 3>;
    // The newest version with a migration below; dev builds write it, so it runs only once there too.
    constexpr Version kNewestMigration = { 1, 8, 0 };
    const std::string kVersionPrefix = "# prvr ", kVersionSuffix = " (do not edit this line!)";
    // "v1.7.2" or "1.7"; false for others, like the dev and commit names of builds outside releases.
    const auto parseVersion = [](std::string text, Version &version)
    {
        if (!text.empty() && text[0] == 'v')
            text.erase(0, 1);
        version = {};
        size_t part = 0, start = 0;
        while (part < version.size())
        {
            const size_t dot = text.find('.', start);
            const std::string number = text.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (number.empty() || number.size() > 6 || number.find_first_not_of("0123456789") != std::string::npos)
                return false;
            version[part++] = std::stoi(number);
            if (dot == std::string::npos)
                return true;
            start = dot + 1;
        }
        return false;
    };
    // The line for a file of fileVersion: never older than it, this build or the newest migration.
    const auto versionLine = [&](const Version &fileVersion)
    {
        Version version = (std::max)(fileVersion, kNewestMigration), own;
        if (parseVersion(PRVR_VERSION, own))
            version = (std::max)(version, own);
        return kVersionPrefix + "v" + std::to_string(version[0]) + "." + std::to_string(version[1]) + "." +
            std::to_string(version[2]) + kVersionSuffix;
    };

    const auto read = [](const char *path, std::string &text)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    };
    // The key as ParseConfigFile reads it; empty for comments and lines without one.
    const auto keyOf = [](const std::string &line)
    {
        const size_t equals = line.find('=');
        return equals == std::string::npos || line[0] == '#' ? std::string() : line.substr(0, equals);
    };

    std::string defaults;
    if (!read("VR\\config.default.txt", defaults))
    {
        std::cout << "VR\\config.default.txt missing, config.txt not checked\n";
        return;
    }
    std::string text;
    if (!read("VR\\config.txt", text))
    {
        std::ofstream file("VR\\config.txt", std::ios::binary);
        file << versionLine({}) << (defaults.find("\r\n") != std::string::npos ? "\r\n" : "\n") << defaults;
        std::cout << (file ? "VR\\config.txt made from config.default.txt" : "Can't make VR\\config.txt") << "\n";
        return;
    }

    // The value as ParseConfigFile reads it.
    const auto valueOf = [](const std::string &line)
    {
        const size_t equals = line.find('=');
        std::string value = line.substr(equals + 1, line.find('#', equals) - equals - 1);
        value.erase(value.find_last_not_of(" \t\r") + 1);
        return value;
    };
    const auto isNumber = [](const std::string &value, float number)
    {
        char *end = nullptr;
        const float parsed = std::strtof(value.c_str(), &end);
        return !value.empty() && *end == '\0' && parsed == number;
    };

    std::unordered_map<std::string, std::string> defaultLines;
    std::vector<std::string> defaultKeys;
    {
        std::istringstream lines(defaults);
        for (std::string line; std::getline(lines, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const std::string key = keyOf(line);
            if (!key.empty() && defaultLines.emplace(key, line).second)
                defaultKeys.push_back(key);
        }
    }

    const char *newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    std::unordered_map<std::string, std::string> values;
    Version fileVersion = {}; // 0.0.0: before the version line, up to v1.7.1
    std::vector<std::string> versionLines; // normally one, the first line
    bool versionLineFirst = false;
    {
        std::istringstream stream(text);
        for (std::string line; std::getline(stream, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.compare(0, kVersionPrefix.size(), kVersionPrefix) == 0)
            {
                if (versionLines.empty())
                {
                    const size_t start = kVersionPrefix.size();
                    parseVersion(line.substr(start, line.find(' ', start) - start), fileVersion);
                    versionLineFirst = lines.empty();
                }
                versionLines.push_back(line);
                continue;
            }
            if (const std::string key = keyOf(line); !key.empty())
                values.emplace(key, valueOf(line));
            lines.push_back(line);
        }
    }
    const std::string newVersionLine = versionLine(fileVersion);

    // v1.7.2: VRScale was 43.2 until v1.6 and 46.1 in v1.6.1 and v1.7; a VRScale still at one of
    // them gets the current default. oldScale: the one the player had, 43.2 if missing.
    const auto scale = values.find("VRScale");
    const bool formerScale = fileVersion < Version{ 1, 7, 2 } && scale != values.end() &&
        (isNumber(scale->second, 43.2f) || isNumber(scale->second, 46.1f));
    const float oldScale = scale == values.end() ? 43.2f : std::strtof(scale->second.c_str(), nullptr);

    // v1.8.0: the portal gun's position was (8, 7.5, -12) from v1.0 to v1.7.2, as
    // ViewmodelPosCustomOffsetX/Y/Z; still there (under the old or new name, a missing one counts
    // as untouched), all three get the current default. Changed by the player: all stay.
    const std::string kGunPosKey = "GunPosCustomOffset";
    bool formerGunPos = fileVersion < Version{ 1, 8, 0 };
    bool anyGunPos = false;
    {
        const char axes[] = { 'X', 'Y', 'Z' };
        const float former[] = { 8.0f, 7.5f, -12.0f };
        for (int i = 0; i < 3; ++i)
        {
            auto value = values.find(kGunPosKey + axes[i]);
            if (value == values.end())
                value = values.find("ViewmodelPosCustomOffset" + std::string(1, axes[i]));
            if (value == values.end())
                continue;
            anyGunPos = true;
            formerGunPos = formerGunPos && isNumber(value->second, former[i]);
        }
    }
    formerGunPos = formerGunPos && anyGunPos;

    // Until v1.6.1 SeatedHeightOffset moved the eyes by source units from the game's eye height
    // (64). BodyHeight takes its place with the eye height the player had, in meters at the
    // VRScale of then. The defaults (43.2 with 4, 46.1 with 8) give the default line (46.1 with 8
    // via 167, below); the VR tab writes all its values, so 4 counts as untouched. Without the key
    // in config.default.txt (older package files) the offset stays.
    // v1.8.0: BodyHeight (Chell's height too) is SeatedBodyHeight of v1.7 to v1.7.2, renamed; its
    // default there, 167, gets the current one (170: Chell's height at VRScale 44.7).
    const std::string kBodyHeightKey = "BodyHeight", kOldBodyHeightKey = "SeatedBodyHeight";
    const auto bodyHeightDefault = defaultLines.find(kBodyHeightKey);
    const bool hasBodyHeight = values.count(kBodyHeightKey) || values.count(kOldBodyHeightKey);
    const bool formerBodyHeight = fileVersion < Version{ 1, 8, 0 };
    const auto bodyHeightLine = [&](const std::string &value)
    {
        const std::string &line = bodyHeightDefault->second;
        const size_t comment = line.find(" #");
        return kBodyHeightKey + "=" + value + (comment == std::string::npos ? "" : line.substr(comment));
    };
    const auto offsetBodyHeightLine = [&](const std::string &offsetText)
    {
        char *end = nullptr;
        const float offset = std::strtof(offsetText.c_str(), &end);
        if (offsetText.empty() || *end != '\0' || !(oldScale > 0) || (oldScale == 43.2f && offset == 4.0f))
            return bodyHeightDefault->second;
        return bodyHeightLine(std::to_string(std::lround((64 + offset) / oldScale * 100 + VR::kEyeBelowCrown)));
    };

    std::string updated = newVersionLine + newline;
    bool changed = !versionLineFirst || versionLines.size() != 1 || versionLines[0] != newVersionLine;
    if (changed)
        std::cout << "config.txt: version line " << newVersionLine << "\n";
    for (std::string &line : lines)
    {
        const std::string key = keyOf(line);
        if (key == "SeatedHeightOffset" && bodyHeightDefault != defaultLines.end())
        {
            changed = true;
            if (hasBodyHeight)
            {
                std::cout << "Config entry 'SeatedHeightOffset' removed, BodyHeight replaces it\n";
                continue;
            }
            line = offsetBodyHeightLine(valueOf(line));
            values.emplace(kBodyHeightKey, valueOf(line));
            std::cout << "Config entry 'SeatedHeightOffset' became BodyHeight=" << valueOf(line) << "\n";
        }
        if (key == kOldBodyHeightKey && bodyHeightDefault != defaultLines.end())
        {
            changed = true;
            if (values.count(kBodyHeightKey))
            {
                std::cout << "Config entry 'SeatedBodyHeight' removed, BodyHeight replaces it\n";
                continue;
            }
            line = bodyHeightLine(valueOf(line));
            values.emplace(kBodyHeightKey, valueOf(line));
            std::cout << "Config entry 'SeatedBodyHeight' became BodyHeight\n";
        }
        if (formerBodyHeight && keyOf(line) == kBodyHeightKey && isNumber(valueOf(line), 167.0f) &&
            bodyHeightDefault != defaultLines.end() && bodyHeightDefault->second != line)
        {
            line = bodyHeightDefault->second;
            changed = true;
            std::cout << "Config entry 'BodyHeight' was at the former default, now " << valueOf(line) << "\n";
        }
        // Until v1.7 the portal gun's offsets were ViewmodelPos/AngCustomOffsetX/Y/Z: the values
        // stay, with the new names and comments.
        const std::string kOldGunPrefix = "Viewmodel";
        if (key.rfind(kOldGunPrefix, 0) == 0)
        {
            const std::string renamed = "Gun" + key.substr(kOldGunPrefix.size());
            const auto current = defaultLines.find(renamed);
            if (current != defaultLines.end())
            {
                changed = true;
                if (values.count(renamed))
                {
                    std::cout << "Config entry '" << key << "' removed, " << renamed << " replaces it\n";
                    continue;
                }
                const size_t comment = current->second.find(" #");
                line = renamed + "=" + valueOf(line) + (comment == std::string::npos ? "" : current->second.substr(comment));
                values.emplace(renamed, valueOf(line));
                std::cout << "Config entry '" << key << "' became " << renamed << "\n";
            }
        }
        if (const std::string name = keyOf(line); formerGunPos && name.rfind(kGunPosKey, 0) == 0)
        {
            const auto current = defaultLines.find(name);
            if (current != defaultLines.end() && current->second != line)
            {
                line = current->second;
                changed = true;
                std::cout << "Config entry '" << name << "' was at the former default, now " << valueOf(line) << "\n";
            }
        }
        const auto current = defaultLines.find(key);
        if (key == "VRScale" && formerScale && current != defaultLines.end() && current->second != line)
        {
            line = current->second;
            changed = true;
            std::cout << "Config entry 'VRScale' was at the former default, now " << valueOf(line) << "\n";
        }
        updated += line + newline;
    }
    if (changed)
        text = updated;

    std::string added;
    for (const std::string &key : defaultKeys)
    {
        if (values.count(key))
            continue;
        added += defaultLines[key] + newline;
        std::cout << "Config entry '" << key << "' added to config.txt\n";
    }
    if (added.empty() && !changed)
        return;
    if (!text.empty() && text.back() != '\n')
        text += newline;
    std::ofstream file("VR\\config.txt", std::ios::binary | std::ios::trunc);
    file << text << added;
    if (!file)
        std::cout << "Can't write VR\\config.txt\n";
}

void VR::ParseConfigFile()
{
    std::ifstream configStream("VR\\config.txt");
    std::unordered_map<std::string, std::string> userConfig;

    std::string line;
    while (std::getline(configStream, line))
    {
        std::istringstream sLine(line);
        std::string key;
        if (std::getline(sLine, key, '='))
        {
            std::string value;
            if (std::getline(sLine, value, '#'))
            {
                // Without this, "true # comment" would not count as true.
                value.erase(value.find_last_not_of(" \t\r") + 1);
                userConfig[key] = value;
            }
        }
    }

    if (userConfig.empty())
        return;

    // Parse a single entry with key 'key' from the config into 'target'.
    // If the entry does not exist, or if the parsing fails, sets 'target' to
    // 'defaultValue'.
    const auto parseOrDefault = [&](const char* key, auto& target,
                                    const auto& defaultValue)
    {
        target = parseConfigEntry(userConfig, *m_Game, key, defaultValue);
        std::cout << "Setting '" << key << "' to '" << target << "'\n";
    };

    // Parses a vector or angle from the config into 'target'. The XYZ coordinates
    // are read from three separate config entries with key 'keyPrefix' + 'X'/'Y'/'Z'.
    const auto parseXYZOrDefault = [&](std::string keyPrefix, auto& target, float x, float y, float z)
    {
        parseOrDefault((keyPrefix + "X").c_str(), target.x, x);
        parseOrDefault((keyPrefix + "Y").c_str(), target.y, y);
        parseOrDefault((keyPrefix + "Z").c_str(), target.z, z);
    };

    parseOrDefault("SnapTurning", m_SnapTurning, true);
    parseOrDefault("SnapTurnAngle", m_SnapTurnAngle, 30.0f);
    parseOrDefault("TurnSpeed", m_TurnSpeed, 0.15f);
    parseOrDefault("LeftHanded", m_LeftHanded, false);
    parseOrDefault("WalkWithRightStick", m_WalkWithRightStick, false);
    parseOrDefault("HeadOrientedWalking", m_HeadOrientedWalking, false);
    parseOrDefault("VRScale", m_VRScale, 44.7f);
    parseOrDefault("IPDScale", m_IpdScale, 1.0f);
    parseOrDefault("6DOF", m_6DOF, true);
    parseOrDefault("SeatedMode", m_SeatedMode, false);
    parseOrDefault("BodyHeight", m_BodyHeight, 170.0f);
    parseOrDefault("BodyCrouch", m_BodyCrouch, true);
    parseOrDefault("CrouchEyeHeight", m_CrouchEyeHeight, 0.75f);
    parseOrDefault("Roomscale", m_Roomscale, true);
    parseOrDefault("AimMode", m_AimMode, 1);
    parseOrDefault("Arms", m_Arms, true);
    parseXYZOrDefault("GunPosCustomOffset", m_GunPosCustomOffset, kGunPosDefaultOffset.x, kGunPosDefaultOffset.y, kGunPosDefaultOffset.z);
    parseXYZOrDefault("GunAngCustomOffset", m_GunAngCustomOffset, 0.0f, 0.0f, 0.0f);
    parseXYZOrDefault("HandPosCustomOffset", m_HandPosCustomOffset, kHandPosDefaultOffset.x, kHandPosDefaultOffset.y, kHandPosDefaultOffset.z);
    parseOrDefault("ApplyPitchAndRollPortalRotationOffset", m_ApplyPitchAndRollPortalRotationOffset, true);
    parseOrDefault("CameraUprightRecoveryTime", m_CameraUprightRecoveryTime, 0.25f);
    parseOrDefault("VignetteWalking", m_VignetteWalking, 0.0f);
    parseOrDefault("VignetteTurning", m_VignetteTurning, 0.8f);
    parseOrDefault("VignetteAfterPortal", m_VignetteAfterPortal, 1.0f);
    parseOrDefault("VignetteClearAngle", m_VignetteClearAngle, 20.0f);
    parseOrDefault("HudDistance", m_HudDistance, 0.8f);
    parseOrDefault("ForceFeedback", m_ForceFeedback, true);
    parseOrDefault("ForceFeedbackPortalGun", m_ForceFeedbackPortalGun, 0.5f);
}

void VR::WaitForConfigUpdate()
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char configDir[MAX_STR_LEN];
    sprintf_s(configDir, MAX_STR_LEN, "%s\\VR\\", currentDir);
    HANDLE fileChangeHandle = FindFirstChangeNotificationA(configDir, false, FILE_NOTIFY_CHANGE_LAST_WRITE);

    std::filesystem::file_time_type configLastModified;
    bool readAlready = true; // by VR::init
    while (1)
    {
        try
        {
            // Windows only notifies of change within a directory, so extra check here for just config.txt
            auto configModifiedTime = std::filesystem::last_write_time("VR\\config.txt");
            if (configModifiedTime != configLastModified)
            {
                configLastModified = configModifiedTime;
                if (!readAlready)
                {
                    ParseConfigFile();
                    std::cout << "Successfully reloaded 'config.txt'\n";
                }
                readAlready = false;
            }
        }
        catch (const std::invalid_argument &e)
        {
            concatErrorMsg(
                *m_Game, "Failed to parse 'config.txt' (", e.what(), ")");
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            concatErrorMsg(
                *m_Game, "'config.txt' not found. (", e.what(), ")");

            return;
        }

        FindNextChangeNotification(fileChangeHandle);
        WaitForSingleObject(fileChangeHandle, INFINITE);
        Sleep(100); // Sometimes the thread tries to read config.txt before it's finished writing
    }
}
