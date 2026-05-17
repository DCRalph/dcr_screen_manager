#pragma once

#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct Screen2;
using ScreenBackgroundStep = void (*)(const Screen2 *screen);

struct ScreenBackgroundTaskConfig
{
  ScreenBackgroundStep step = nullptr;
  const char *name = nullptr;
  uint32_t stackDepth = 4096;
  uint32_t loopDelayMs = 1000;
  uint32_t watchdogTimeoutMs = 15000;
};

struct Screen2
{
  const char *name;
  void (*draw)();
  void (*update)();
  void (*onEnter)();
  void (*onExit)();
  ScreenBackgroundTaskConfig backgroundTask{};
};

class ScreenManager
{
public:
  // Hooks let the host application plug in project-specific behavior.
  using ScreenPredicate = bool (*)(const Screen2 *screen);
  using StuckHandler = void (*)(const Screen2 *currentScreen);
  using BoolProvider = bool (*)();
  using VoidCallback = void (*)();

private:
  const Screen2 *currentScreen = nullptr;
  const Screen2 *pendingScreen = nullptr;
  std::vector<const Screen2 *> screenHistory;

  // Button checking state
  unsigned long lastButtonCheckTime = 0;
  const Screen2 *lastCheckedScreen = nullptr;
  bool popupShownForCurrentScreen = false;
  static const unsigned long BUTTON_CHECK_INTERVAL_MS = 10000; // 10 seconds

  // Background task state (one task per active screen)
  TaskHandle_t backgroundTaskHandle = nullptr;
  TaskHandle_t backgroundWatchdogHandle = nullptr;
  const Screen2 *backgroundTaskOwner = nullptr;
  ScreenBackgroundStep backgroundTaskStep = nullptr;
  uint32_t backgroundTaskLoopDelayMs = 1000;
  uint32_t backgroundTaskWatchdogTimeoutMs = 15000;
  volatile bool backgroundTaskShouldStop = false;
  volatile bool backgroundTaskStepInProgress = false;
  volatile uint32_t backgroundTaskStepStartMs = 0;

  // Pluggable host hooks.
  ScreenPredicate exceptionScreenCheck = nullptr;
  ScreenPredicate controlCenterBlockedCheck = nullptr;
  StuckHandler stuckHandler = nullptr;
  BoolProvider activeButtonsCheck = nullptr;
  BoolProvider popupActiveCheck = nullptr;
  VoidCallback clearActiveButtonsFn = nullptr;

  void updateHistory(const Screen2 *screen);
  bool isExceptionScreen(const Screen2 *screen);
  void checkForNoButtons();
  bool startRegisteredBackgroundTaskForCurrentScreen();
  void ensureRegisteredBackgroundTaskRunning();
  static void backgroundTaskRunner(void *pvParameters);
  static void backgroundTaskWatchdogRunner(void *pvParameters);

public:
  void init();

  void update(void);
  void draw(void);

  const Screen2 *getCurrentScreen(void);

  /// True when the host indicates the control center must not be shown
  /// (boot, fatal error, OTA in progress, etc.). Driven by the registered
  /// controlCenterBlockedCheck hook.
  bool isControlCenterBlocked() const;

  void setScreen(const Screen2 *screen);
  bool applyPendingScreenChange();

  void back(void);
  void clearHistory(void);
  bool goToHistoryIndex(size_t index);
  void resetPopupShownFlag(void);

  // Background task support (one task per active screen)
  bool startBackgroundTask(ScreenBackgroundStep stepCode,
                           const char *name,
                           uint32_t stackDepth,
                           uint32_t loopDelayMs = 1000,
                           uint32_t watchdogTimeoutMs = 15000);
  bool stopBackgroundTask(bool waitForStop = false,
                          uint32_t timeoutMs = 0);
  bool isBackgroundTaskActive(const Screen2 *screen);

  // Hook registration
  void setExceptionScreenCheck(ScreenPredicate fn) { exceptionScreenCheck = fn; }
  void setControlCenterBlockedCheck(ScreenPredicate fn) { controlCenterBlockedCheck = fn; }
  void setStuckHandler(StuckHandler fn) { stuckHandler = fn; }
  void setActiveButtonsCheck(BoolProvider fn) { activeButtonsCheck = fn; }
  void setPopupActiveCheck(BoolProvider fn) { popupActiveCheck = fn; }
  void setClearActiveButtons(VoidCallback fn) { clearActiveButtonsFn = fn; }
};

extern ScreenManager screenManager;
