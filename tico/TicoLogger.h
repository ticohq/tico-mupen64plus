/// @file TicoLogger.h
/// @brief Category-filtered logging system with file output on Switch
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

#ifdef __SWITCH__
#include <sys/stat.h>
#endif

/// @brief Singleton logger with per-category filtering and log levels
class Logger {
public:
  /// @brief Log severity levels
  enum class Level { DEBUG, INFO, WARNING, ERROR };

  /// @brief Flags to control log output behavior
  enum Flags : uint32_t {
    None = 0,
    SwitchOnly = 1 << 0
  };

  static Logger &Instance() {
    static Logger instance;
    return instance;
  }

  /// @brief Enable or disable a logging category at runtime
  void EnableCategory(const std::string &category, bool enabled = true) {
    m_categoryStates[category] = enabled;
  }

  void DisableCategory(const std::string &category) {
    m_categoryStates[category] = false;
  }

  bool IsCategoryEnabled(const std::string &category) const {
    auto it = m_categoryStates.find(category);
    if (it != m_categoryStates.end()) {
      return it->second;
    }
    return true;
  }

  void Log(Level level, const char *category, uint32_t flags,
           const char *format, ...) {
#ifdef DISABLE_LOGGING
    return;
#endif
    if (flags & SwitchOnly) {
#ifndef __SWITCH__
      return; // Skip if not on Switch
#endif
    }

    if (!IsCategoryEnabled(category)) {
      return;
    }

#ifdef __SWITCH__
    if (!m_file) {
      mkdir("sdmc:/tico", 0777);
      mkdir("sdmc:/tico/debug", 0777);
      m_file = fopen(kSwitchLogPath, "a");
    }
    if (!m_file)
      return;

    fprintf(m_file, "[%s][%s] ", LevelToString(level), category);

    va_list args;
    va_start(args, format);
    vfprintf(m_file, format, args);
    va_end(args);

    fprintf(m_file, "\n");
    fflush(m_file);
#else
    printf("[%s][%s] ", LevelToString(level), category);

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    printf("\n");
#endif
  }

  /// @brief Set minimum log level threshold
  void SetMinLevel(Level level) { m_minLevel = level; }

  void StartNewLogFile() {
#if defined(__SWITCH__) && !defined(DISABLE_LOGGING)
    ResetLogFile();
    mkdir("sdmc:/tico", 0777);
    mkdir("sdmc:/tico/debug", 0777);
    m_file = fopen(kSwitchLogPath, "w");
    if (m_file) {
      fprintf(m_file, "=== Tico mupen64plus log start ===\n");
      fflush(m_file);
    }
#endif
  }

  void ResetLogFile() {
#ifdef __SWITCH__
    if (m_file) {
      fflush(m_file);
      fclose(m_file);
      m_file = nullptr;
    }
#endif
  }

  void CloseLogFile() { ResetLogFile(); }

  bool ShouldLog(Level level) const {
#ifdef DISABLE_LOGGING
    return false;
#endif
    return static_cast<int>(level) >= static_cast<int>(m_minLevel);
  }

  ~Logger() {
    ResetLogFile();
  }

private:
  Logger() : m_minLevel(Level::DEBUG) {
#ifdef __SWITCH__
    m_file = nullptr;
#endif
    InitializeCategories();
  }

  static constexpr const char *kSwitchLogPath =
      "sdmc:/tico/debug/mupen64plus.txt";

  void InitializeCategories() {
#ifdef _DEBUG
    m_categoryStates["EGL"] = true;
    m_categoryStates["VK"] = true;
    m_categoryStates["CORE"] = true;
    m_categoryStates["RENDER"] = true;
    m_categoryStates["INPUT"] = true;
    m_categoryStates["AUDIO"] = true;
    m_categoryStates["LOADER"] = true;
    m_categoryStates["EMULATOR"] = true;
    m_categoryStates["HOME"] = true;
    m_categoryStates["OVERLAY"] = true;
#else
    m_categoryStates["EGL"] = true;
    m_categoryStates["VK"] = true;
    m_categoryStates["CORE"] = true;
    m_categoryStates["RENDER"] = true;
    m_categoryStates["INPUT"] = false;
    m_categoryStates["AUDIO"] = false;
    m_categoryStates["LOADER"] = true;
    m_categoryStates["EMULATOR"] = true;
    m_categoryStates["HOME"] = true;
    m_categoryStates["OVERLAY"] = true;
#endif
  }

  const char *LevelToString(Level level) const {
    switch (level) {
    case Level::DEBUG:
      return "DEBUG";
    case Level::INFO:
      return "INFO";
    case Level::WARNING:
      return "WARN";
    case Level::ERROR:
      return "ERROR";
    default:
      return "UNKNOWN";
    }
  }

  std::unordered_map<std::string, bool> m_categoryStates;
  Level m_minLevel;

#ifdef __SWITCH__
  FILE *m_file;
#endif
};

/// @name Log macros
/// @brief Convenience macros with category filtering
/// @{
#ifdef DISABLE_LOGGING
#define LOG_DEBUG(cat, ...)                                                    \
  do {                                                                         \
  } while (0)
#define LOG_INFO(cat, ...)                                                     \
  do {                                                                         \
  } while (0)
#define LOG_WARN(cat, ...)                                                     \
  do {                                                                         \
  } while (0)
#define LOG_ERROR(cat, ...)                                                    \
  do {                                                                         \
  } while (0)
#define LOG_DEBUG_FLAGS(flags, cat, ...)                                       \
  do {                                                                         \
  } while (0)
#define LOG_INFO_FLAGS(flags, cat, ...)                                        \
  do {                                                                         \
  } while (0)
#define LOG_WARN_FLAGS(flags, cat, ...)                                        \
  do {                                                                         \
  } while (0)
#define LOG_ERROR_FLAGS(flags, cat, ...)                                       \
  do {                                                                         \
  } while (0)
#else
#define LOG_DEBUG(cat, ...)                                                    \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::DEBUG)) {                  \
      Logger::Instance().Log(Logger::Level::DEBUG, cat, Logger::None,          \
                             __VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

#define LOG_INFO(cat, ...)                                                     \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::INFO)) {                   \
      Logger::Instance().Log(Logger::Level::INFO, cat, Logger::None,           \
                             __VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

#define LOG_WARN(cat, ...)                                                     \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::WARNING)) {                \
      Logger::Instance().Log(Logger::Level::WARNING, cat, Logger::None,        \
                             __VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

#define LOG_ERROR(cat, ...)                                                    \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::ERROR)) {                  \
      Logger::Instance().Log(Logger::Level::ERROR, cat, Logger::None,          \
                             __VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

/// @name Flag-aware log macros
#define LOG_DEBUG_FLAGS(flags, cat, ...)                                       \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::DEBUG)) {                  \
      Logger::Instance().Log(Logger::Level::DEBUG, cat, flags, __VA_ARGS__);   \
    }                                                                          \
  } while (0)

#define LOG_INFO_FLAGS(flags, cat, ...)                                        \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::INFO)) {                   \
      Logger::Instance().Log(Logger::Level::INFO, cat, flags, __VA_ARGS__);    \
    }                                                                          \
  } while (0)

#define LOG_WARN_FLAGS(flags, cat, ...)                                        \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::WARNING)) {                \
      Logger::Instance().Log(Logger::Level::WARNING, cat, flags, __VA_ARGS__); \
    }                                                                          \
  } while (0)

#define LOG_ERROR_FLAGS(flags, cat, ...)                                       \
  do {                                                                         \
    if (Logger::Instance().IsCategoryEnabled(cat) &&                           \
        Logger::Instance().ShouldLog(Logger::Level::ERROR)) {                  \
      Logger::Instance().Log(Logger::Level::ERROR, cat, flags, __VA_ARGS__);   \
    }                                                                          \
  } while (0)
#endif

/// @name Category-specific shorthand macros
#define LOG_EGL(...) LOG_DEBUG("EGL", __VA_ARGS__)
#define LOG_CORE(...) LOG_DEBUG("CORE", __VA_ARGS__)
#define LOG_RENDER(...) LOG_DEBUG("RENDER", __VA_ARGS__)
#define LOG_INPUT(...) LOG_DEBUG("INPUT", __VA_ARGS__)
#define LOG_AUDIO(...) LOG_DEBUG("AUDIO", __VA_ARGS__)
