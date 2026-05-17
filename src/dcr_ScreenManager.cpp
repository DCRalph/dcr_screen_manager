#include "dcr_ScreenManager.h"
#include <dcr_Logger.h>
#include <dcr_TimeProfiler.h>

#undef LOG_TAG
#define LOG_TAG "SCREEN"

void ScreenManager::init()
{
  pendingScreen = nullptr;
  lastButtonCheckTime = 0;
  lastCheckedScreen = nullptr;
  popupShownForCurrentScreen = false;
  backgroundTaskHandle = nullptr;
  backgroundWatchdogHandle = nullptr;
  backgroundTaskOwner = nullptr;
  backgroundTaskStep = nullptr;
  backgroundTaskShouldStop = false;
  backgroundTaskStepInProgress = false;
  backgroundTaskStepStartMs = 0;
  debugI("Initialized");
}

void ScreenManager::update(void)
{
  timeProfiler.start("screenManager.update");
  timeProfiler.increment("screenManager.update");
  ensureRegisteredBackgroundTaskRunning();
  if (currentScreen && currentScreen->update)
  {
    currentScreen->update();
  }

  if (applyPendingScreenChange())
  {
    update();
  }

  // Check for no buttons every 10 seconds
  checkForNoButtons();

  timeProfiler.stop("screenManager.update");
}

void ScreenManager::draw(void)
{
  timeProfiler.start("screenManager.draw");
  timeProfiler.increment("screenManager.draw");

  if (currentScreen && currentScreen->draw)
  {
    currentScreen->draw();
  }
  timeProfiler.stop("screenManager.draw");
}

const Screen2 *ScreenManager::getCurrentScreen(void)
{
  if (pendingScreen)
    return pendingScreen;
  return currentScreen;
}

bool ScreenManager::isControlCenterBlocked() const
{
  const Screen2 *screen = pendingScreen != nullptr ? pendingScreen : currentScreen;
  if (screen == nullptr)
  {
    return false;
  }
  if (controlCenterBlockedCheck != nullptr)
  {
    return controlCenterBlockedCheck(screen);
  }
  return false;
}

bool ScreenManager::applyPendingScreenChange()
{
  if (pendingScreen != nullptr)
  {
    if (pendingScreen->draw == nullptr || pendingScreen->update == nullptr)
    {
      debugE("Pending screen has no draw or update function");
      pendingScreen = nullptr;
      return false;
    }

    if (currentScreen == pendingScreen)
    {
      debugI("Same screen, skipping");
      pendingScreen = nullptr;
      return false;
    }

    // Request the current screen's background task to stop, but do not block the
    // UI thread while it drains. This keeps the screen responsive during teardown.
    if (backgroundTaskHandle != nullptr)
    {
      stopBackgroundTask(false);
      return false;
    }

    // Actually change the screen
    if (currentScreen && currentScreen->onExit)
    {
      currentScreen->onExit();
    }

    currentScreen = pendingScreen;
    pendingScreen = nullptr;

    debugI(">> %s", currentScreen->name);

    updateHistory(currentScreen);

    if (currentScreen && currentScreen->onEnter)
    {
      currentScreen->onEnter();
    }

    startRegisteredBackgroundTaskForCurrentScreen();

    // Reset button check state when screen changes
    lastButtonCheckTime = millis();
    lastCheckedScreen = currentScreen;
    popupShownForCurrentScreen = false;

    // Let host clear its active-button tracking when the screen changes.
    if (clearActiveButtonsFn != nullptr)
    {
      clearActiveButtonsFn();
    }

    return true;
  }

  return false;
}

// Immediate screen change (use with caution)
void ScreenManager::setScreen(const Screen2 *screen)
{
  pendingScreen = screen;
}

void ScreenManager::back(void)
{
  if (screenHistory.size() > 1)
  {
    screenHistory.pop_back();
    pendingScreen = screenHistory.back();
    debugI("Going back to: %s", pendingScreen->name);
  }
  else
  {
    debugI("Cannot go back - no screen history available");
  }
}

void ScreenManager::clearHistory(void)
{
  debugI("Clearing screen history");

  const Screen2 *current = nullptr;
  if (screenHistory.size() > 0)
  {
    current = screenHistory.back();
  }

  screenHistory.clear();

  if (current != nullptr)
  {
    screenHistory.push_back(current);
  }
}

void ScreenManager::updateHistory(const Screen2 *screen)
{
  if (screen == nullptr)
  {
    debugW("Attempted to add null screen to history");
    return;
  }

  if (screenHistory.empty() || screenHistory.back() != screen)
  {
    screenHistory.push_back(screen);

    if (screenHistory.size() > 10)
    {
      screenHistory.erase(screenHistory.begin());
    }
  }
}

bool ScreenManager::goToHistoryIndex(size_t index)
{
  if (index < screenHistory.size())
  {
    debugI("Navigating to history index %d: %s", index, screenHistory[index]->name);

    if (index != screenHistory.size() - 1)
    {
      pendingScreen = screenHistory[index];
      screenHistory.resize(index + 1);
      return true;
    }

    debugI("Already at requested screen");
    return false;
  }

  debugW("Invalid history index: %d, max: %d", index,
         screenHistory.size() ? screenHistory.size() - 1 : 0);
  return false;
}

bool ScreenManager::isExceptionScreen(const Screen2 *screen)
{
  if (screen == nullptr)
    return true;

  if (exceptionScreenCheck != nullptr)
  {
    return exceptionScreenCheck(screen);
  }
  return false;
}

void ScreenManager::checkForNoButtons()
{
  if (!currentScreen)
    return;

  if (isExceptionScreen(currentScreen))
    return;

  // Skip if the device has been booted in the last 30 seconds
  if (millis() < 30 * 1000)
    return;

  if (popupShownForCurrentScreen)
    return;

  // Skip if host reports a popup is currently active.
  if (popupActiveCheck != nullptr && popupActiveCheck())
    return;

  unsigned long currentTime = millis();
  if (currentTime - lastButtonCheckTime < BUTTON_CHECK_INTERVAL_MS)
    return;

  lastButtonCheckTime = currentTime;
  lastCheckedScreen = currentScreen;

  // Without a host-provided button check, we cannot judge.
  if (activeButtonsCheck == nullptr)
    return;

  if (!activeButtonsCheck())
  {
    debugI("No active buttons detected on screen '%s', notifying host",
           currentScreen->name);

    popupShownForCurrentScreen = true;

    if (stuckHandler != nullptr)
    {
      stuckHandler(currentScreen);
    }
  }
}

void ScreenManager::resetPopupShownFlag(void)
{
  popupShownForCurrentScreen = false;
  lastButtonCheckTime = millis();
}

bool ScreenManager::startRegisteredBackgroundTaskForCurrentScreen()
{
  if (currentScreen == nullptr)
  {
    return false;
  }

  const ScreenBackgroundTaskConfig &taskConfig = currentScreen->backgroundTask;
  if (taskConfig.step == nullptr)
  {
    return false;
  }

  if (isBackgroundTaskActive(currentScreen))
  {
    return true;
  }

  if (backgroundTaskHandle != nullptr)
  {
    debugW("Background task already active while entering '%s'", currentScreen->name);
    return false;
  }

  const char *taskName = (taskConfig.name != nullptr) ? taskConfig.name : currentScreen->name;
  bool started = startBackgroundTask(taskConfig.step,
                                     taskName,
                                     taskConfig.stackDepth,
                                     taskConfig.loopDelayMs,
                                     taskConfig.watchdogTimeoutMs);
  if (!started)
  {
    debugE("Failed to start registered background task for '%s'", currentScreen->name);
  }

  return started;
}

void ScreenManager::ensureRegisteredBackgroundTaskRunning()
{
  if (pendingScreen != nullptr)
  {
    return;
  }

  if (currentScreen == nullptr)
  {
    return;
  }

  if (currentScreen->backgroundTask.step == nullptr)
  {
    return;
  }

  if (isBackgroundTaskActive(currentScreen))
  {
    return;
  }

  if (backgroundTaskHandle != nullptr)
  {
    return;
  }

  debugW("Restarting registered background task for '%s'", currentScreen->name);
  startRegisteredBackgroundTaskForCurrentScreen();
}

void ScreenManager::backgroundTaskRunner(void *pvParameters)
{
  ScreenManager *manager = static_cast<ScreenManager *>(pvParameters);
  if (manager == nullptr)
  {
    vTaskDelete(NULL);
    return;
  }

  const Screen2 *ownerScreen = manager->backgroundTaskOwner;
  while (manager->isBackgroundTaskActive(ownerScreen))
  {
    if (manager->backgroundTaskStep == nullptr)
    {
      break;
    }

    manager->backgroundTaskStepInProgress = true;
    manager->backgroundTaskStepStartMs = millis();
    manager->backgroundTaskStep(ownerScreen);
    manager->backgroundTaskStepInProgress = false;

    const uint32_t waitStepMs = 50;
    uint32_t waitedMs = 0;
    while (waitedMs < manager->backgroundTaskLoopDelayMs && manager->isBackgroundTaskActive(ownerScreen))
    {
      vTaskDelay(pdMS_TO_TICKS(waitStepMs));
      waitedMs += waitStepMs;
    }
  }

  manager->backgroundTaskStepInProgress = false;
  manager->backgroundTaskHandle = nullptr;
  manager->backgroundTaskOwner = nullptr;
  manager->backgroundTaskStep = nullptr;
  manager->backgroundTaskShouldStop = false;
  manager->backgroundTaskStepStartMs = 0;

  if (manager->backgroundWatchdogHandle != nullptr)
  {
    vTaskDelete(manager->backgroundWatchdogHandle);
    manager->backgroundWatchdogHandle = nullptr;
  }

  vTaskDelete(NULL);
}

void ScreenManager::backgroundTaskWatchdogRunner(void *pvParameters)
{
  ScreenManager *manager = static_cast<ScreenManager *>(pvParameters);
  if (manager == nullptr)
  {
    vTaskDelete(NULL);
    return;
  }

  while (manager->backgroundTaskHandle != nullptr)
  {
    if (manager->backgroundTaskStepInProgress)
    {
      uint32_t elapsedMs = millis() - manager->backgroundTaskStepStartMs;
      if (elapsedMs > manager->backgroundTaskWatchdogTimeoutMs)
      {
        debugE("Background task watchdog timeout (%lums). Requesting stop.", elapsedMs);
        manager->backgroundTaskShouldStop = true;
        break;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }

  manager->backgroundWatchdogHandle = nullptr;
  vTaskDelete(NULL);
}

bool ScreenManager::startBackgroundTask(ScreenBackgroundStep stepCode,
                                        const char *name,
                                        uint32_t stackDepth,
                                        uint32_t loopDelayMs,
                                        uint32_t watchdogTimeoutMs)
{
  if (backgroundTaskHandle != nullptr || currentScreen == nullptr || stepCode == nullptr)
  {
    return false;
  }

  backgroundTaskShouldStop = false;
  backgroundTaskOwner = currentScreen;
  backgroundTaskStep = stepCode;
  backgroundTaskLoopDelayMs = loopDelayMs;
  backgroundTaskWatchdogTimeoutMs = watchdogTimeoutMs;
  backgroundTaskStepInProgress = false;
  backgroundTaskStepStartMs = 0;

  BaseType_t created = xTaskCreate(ScreenManager::backgroundTaskRunner, name, stackDepth, this, 1, &backgroundTaskHandle);
  if (created != pdPASS)
  {
    backgroundTaskOwner = nullptr;
    backgroundTaskStep = nullptr;
    backgroundTaskHandle = nullptr;
    backgroundTaskShouldStop = false;
    return false;
  }

  BaseType_t watchdogCreated = xTaskCreate(ScreenManager::backgroundTaskWatchdogRunner, "screenBgWdog", 3072, this, 2, &backgroundWatchdogHandle);
  if (watchdogCreated != pdPASS)
  {
    TaskHandle_t taskToKill = backgroundTaskHandle;
    backgroundTaskHandle = nullptr;
    backgroundTaskOwner = nullptr;
    backgroundTaskStep = nullptr;
    backgroundTaskShouldStop = true;
    if (taskToKill != nullptr)
    {
      vTaskDelete(taskToKill);
    }
    return false;
  }

  return true;
}

bool ScreenManager::stopBackgroundTask(bool waitForStop, uint32_t timeoutMs)
{
  backgroundTaskShouldStop = true;

  TaskHandle_t taskHandle = backgroundTaskHandle;
  if (!waitForStop || taskHandle == nullptr)
  {
    return true;
  }

  if (taskHandle == xTaskGetCurrentTaskHandle())
  {
    debugW("stopBackgroundTask(wait=true) called from the background task");
    return false;
  }

  const uint32_t waitDeadlineMs = millis() + timeoutMs;
  while (backgroundTaskHandle != nullptr)
  {
    if (timeoutMs > 0 && static_cast<int32_t>(millis() - waitDeadlineMs) >= 0)
    {
      debugE("Background task did not stop within %lums", timeoutMs);
      return false;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }

  return true;
}

bool ScreenManager::isBackgroundTaskActive(const Screen2 *screen)
{
  return (backgroundTaskHandle != nullptr) && (backgroundTaskOwner == screen) && !backgroundTaskShouldStop;
}

ScreenManager screenManager;
