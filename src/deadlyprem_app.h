// deadlyprem - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <memory>

#include <rex/rex_app.h>

#include "deadlyprem_fp_guard.h"

class DeadlypremApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DeadlypremApp>(new DeadlypremApp(ctx, "deadlyprem",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& /*config*/) override {
#ifdef _WIN32
    veh_handle_ = InstallGuestFpExceptionHandlerWin();
#endif
  }

  void OnPostSetup() override {
#ifndef _WIN32
    veh_handle_ = InstallGuestFpExceptionHandlerPosix();
#endif
  }

  void OnShutdown() override {
    RemoveGuestFpExceptionHandler(veh_handle_);
    veh_handle_ = nullptr;
  }

 private:
  void* veh_handle_ = nullptr;
};
