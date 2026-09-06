/**
 * @file        ui/overlay/acquire_wizard_overlay.cpp
 *
 * @brief       Generic pre-runtime acquisition dialog.
 */
#include "deadlyprem_acquire_wizard.h"

#include <algorithm>
#include <utility>

#include <imgui.h>

namespace deadlyprem {

AcquireWizardDialog::AcquireWizardDialog(ImGuiDrawer* drawer, Options options, FetchCallback fetch,
                                         PickSourceCallback pick_source, InstallCallback install,
                                         CompleteCallback complete)
    : ImGuiDialog(drawer),
      options_(std::move(options)),
      fetch_(std::move(fetch)),
      pick_source_(std::move(pick_source)),
      install_(std::move(install)),
      complete_(std::move(complete)),
      status_(options_.initial_status) {}

void AcquireWizardDialog::OnClose() {
  if (work_thread_.joinable()) {
    work_thread_.join();
  }
}

void AcquireWizardDialog::StartWork(std::function<bool(std::string&)> work,
                                    std::string busy_status) {
  if (work_thread_.joinable()) {
    work_thread_.join();
  }
  copied_bytes_ = 0;
  total_bytes_ = 0;
  work_done_ = false;
  work_ok_ = false;
  error_.clear();
  state_ = State::kWorking;
  status_ = std::move(busy_status);
  work_thread_ = std::thread([this, work = std::move(work)]() {
    std::string error;
    const bool ok = work(error);
    error_ = std::move(error);
    work_ok_ = ok;
    work_done_ = true;
  });
}

void AcquireWizardDialog::StartFetch() {
  if (!fetch_) {
    return;
  }
  source_path_.clear();
  working_is_fetch_ = true;
  StartWork([this](std::string& error) { return fetch_(copied_bytes_, total_bytes_, error); },
            options_.fetch_working_status);
}

void AcquireWizardDialog::PickSourceAndInstall() {
  if (!pick_source_ || !install_) {
    return;
  }
  auto source_path = pick_source_();
  if (source_path.empty()) {
    return;
  }
  source_path_ = std::move(source_path);
  working_is_fetch_ = false;
  StartWork(
      [this](std::string& error) {
        return install_(source_path_, copied_bytes_, total_bytes_, error);
      },
      options_.install_working_status);
}

const std::string& AcquireWizardDialog::WorkingStatus() const {
  // Before any bytes arrive during a fetch, the connection may be waiting on
  // the server's first byte; surface that instead of an idle "downloading".
  if (working_is_fetch_ && !options_.fetch_connecting_status.empty() &&
      copied_bytes_.load(std::memory_order_relaxed) == 0) {
    return options_.fetch_connecting_status;
  }
  return status_;
}

void AcquireWizardDialog::FinishWorkIfNeeded() {
  if (state_ != State::kWorking || !work_done_.load(std::memory_order_acquire)) {
    return;
  }

  if (work_thread_.joinable()) {
    work_thread_.join();
  }

  if (work_ok_.load(std::memory_order_acquire)) {
    state_ = State::kDone;
    status_ = options_.done_status;
  } else {
    state_ = State::kFailed;
    status_ = options_.initial_status;
  }
}

void AcquireWizardDialog::OnDraw(ImGuiIO& io) {
  FinishWorkIfNeeded();

  const float width = std::min(760.0f, std::max(460.0f, io.DisplaySize.x - 64.0f));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 22.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 9.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 12.0f));
  ImGui::PushFont(nullptr, 18.0f);

  if (ImGui::Begin(options_.title.c_str(), nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    if (!options_.intro.empty()) {
      ImGui::TextWrapped("%s", options_.intro.c_str());
      ImGui::Spacing();
    }
    if (!options_.target_directory.empty()) {
      ImGui::TextWrapped("Install directory: %s", options_.target_directory.c_str());
    }
    ImGui::TextWrapped("%s", WorkingStatus().c_str());

    if (!source_path_.empty()) {
      ImGui::TextWrapped("Source: %s", source_path_.string().c_str());
    }

    if (state_ == State::kWorking) {
      const uint64_t total = total_bytes_.load(std::memory_order_relaxed);
      const uint64_t copied = copied_bytes_.load(std::memory_order_relaxed);
      const float progress =
          total == 0 ? 0.0f
                     : std::clamp(static_cast<float>(double(copied) / double(total)), 0.0f, 1.0f);
      ImGui::ProgressBar(progress, ImVec2(-1.0f, 28.0f));
    } else if (state_ == State::kFailed) {
      ImGui::TextColored(ImVec4(0.95f, 0.28f, 0.24f, 1.0f), "%s", error_.c_str());
    }

    ImGui::Spacing();
    // dpour-fork 2026-09-02: size buttons from their label instead of fixed
    // widths - the ISO installer's "Select disc image..." clipped at 180 px.
    auto button_width = [](const std::string& label, float min_width) {
      const float text_width = ImGui::CalcTextSize(label.c_str()).x;
      return std::max(min_width, text_width + ImGui::GetStyle().FramePadding.x * 2.0f + 24.0f);
    };
    if (state_ == State::kWaitingForChoice || state_ == State::kFailed) {
      bool first_button = true;
      if (fetch_ && !options_.fetch_button_label.empty()) {
        if (ImGui::Button(options_.fetch_button_label.c_str(),
                          ImVec2(button_width(options_.fetch_button_label, 220.0f), 42.0f))) {
          StartFetch();
        }
        first_button = false;
      }
      if (pick_source_ && install_ && !options_.pick_button_label.empty()) {
        if (!first_button) {
          ImGui::SameLine();
        }
        if (ImGui::Button(options_.pick_button_label.c_str(),
                          ImVec2(button_width(options_.pick_button_label, 180.0f), 42.0f))) {
          PickSourceAndInstall();
        }
      }
    } else if (state_ == State::kDone) {
      if (ImGui::Button(options_.done_button_label.c_str(),
                        ImVec2(button_width(options_.done_button_label, 160.0f), 42.0f))) {
        auto complete = std::move(complete_);
        Close();
        if (complete) {
          complete();
        }
      }
    }

    ImGui::End();
  }

  ImGui::PopFont();
  ImGui::PopStyleVar(3);
}

}  // namespace deadlyprem
