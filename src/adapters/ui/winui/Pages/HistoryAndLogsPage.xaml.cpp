#include "pch.h"

#include "HistoryAndLogsPage.xaml.h"

#include <optional>
#include <string>
#include <string_view>

#include "azzs/application/history_and_logs.hpp"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>

#if __has_include("Pages/HistoryAndLogsPage.g.cpp")
#include "Pages/HistoryAndLogsPage.g.cpp"
#endif

namespace winrt::Azzs::Ui::Pages::implementation {
namespace {

using azzs::application::ExecutionEventKind;
using azzs::application::ExecutionResult;
using azzs::application::HistoryAndLogsActionCode;
using azzs::application::HistoryAndLogsFilter;
using azzs::application::HistoryAndLogsSnapshot;
using azzs::application::HistoryEntryKind;
using azzs::application::HistoryFactDisposition;
using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
using winrt::Microsoft::UI::Xaml::Controls::ContentDialog;
using winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult;
using winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity;
using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

[[nodiscard]] winrt::hstring resource_string(wchar_t const* key) {
  return ResourceLoader{}.GetString(key);
}

void replace_token(std::wstring& value, std::wstring_view token,
                   std::wstring_view replacement) {
  auto const position = value.find(token);
  if (position != std::wstring::npos) {
    value.replace(position, token.size(), replacement);
  }
}

[[nodiscard]] std::wstring coverage_time_text(
    std::optional<azzs::application::WallClockTime> const& value) {
  if (!value.has_value()) {
    return std::wstring{resource_string(L"HistoryAndLogsNotObtained")};
  }
  return std::to_wstring(value->time_since_epoch().count());
}

[[nodiscard]] winrt::hstring history_kind_text(HistoryEntryKind kind) {
  switch (kind) {
    case HistoryEntryKind::installation_batch:
      return resource_string(L"HistoryAndLogsHistoryKindInstallationBatch");
    case HistoryEntryKind::software_optimization_batch:
      return resource_string(
          L"HistoryAndLogsHistoryKindSoftwareOptimizationBatch");
    case HistoryEntryKind::system_setting_apply:
      return resource_string(L"HistoryAndLogsHistoryKindSystemSettingApply");
    case HistoryEntryKind::system_setting_recovery:
      return resource_string(
          L"HistoryAndLogsHistoryKindSystemSettingRecovery");
    case HistoryEntryKind::external_install_handoff:
      return resource_string(L"HistoryAndLogsHistoryKindExternalInstallHandoff");
    case HistoryEntryKind::restart_resume:
      return resource_string(L"HistoryAndLogsHistoryKindRestartResume");
    case HistoryEntryKind::application_update:
      return resource_string(L"HistoryAndLogsHistoryKindApplicationUpdate");
  }
  return resource_string(L"HistoryAndLogsHistoryKindUnknown");
}

[[nodiscard]] winrt::hstring event_kind_text(ExecutionEventKind kind) {
  switch (kind) {
    case ExecutionEventKind::user_command:
      return resource_string(L"HistoryAndLogsEventKindUserCommand");
    case ExecutionEventKind::state_transition:
      return resource_string(L"HistoryAndLogsEventKindStateTransition");
    case ExecutionEventKind::adapter_result:
      return resource_string(L"HistoryAndLogsEventKindAdapterResult");
    case ExecutionEventKind::coverage_gap:
      return resource_string(L"HistoryAndLogsEventKindCoverageGap");
  }
  return resource_string(L"HistoryAndLogsEventKindUnknown");
}

[[nodiscard]] winrt::hstring result_text(ExecutionResult result) {
  switch (result) {
    case ExecutionResult::started:
      return resource_string(L"HistoryAndLogsResultStarted");
    case ExecutionResult::succeeded:
      return resource_string(L"HistoryAndLogsResultSucceeded");
    case ExecutionResult::failed:
      return resource_string(L"HistoryAndLogsResultFailed");
    case ExecutionResult::cancelled:
      return resource_string(L"HistoryAndLogsResultCancelled");
    case ExecutionResult::unknown:
      return resource_string(L"HistoryAndLogsResultUnknown");
  }
  return resource_string(L"HistoryAndLogsResultFallback");
}

void append_row(StackPanel const& target, winrt::hstring const& title,
                winrt::hstring const& state,
                winrt::hstring const& detail = {}) {
  auto row = StackPanel{};
  row.Spacing(2);
  auto title_text = TextBlock{};
  title_text.Text(title);
  title_text.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  title_text.FontWeight({600});
  row.Children().Append(title_text);
  auto state_text = TextBlock{};
  state_text.Text(state);
  state_text.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  row.Children().Append(state_text);
  if (!detail.empty()) {
    auto detail_text = TextBlock{};
    detail_text.Text(detail);
    detail_text.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    row.Children().Append(detail_text);
  }
  AutomationProperties::SetName(row, title + L", " + state);
  target.Children().Append(row);
}

[[nodiscard]] std::string history_detail_text(
    azzs::application::HistoryEntryProjection const& entry) {
  std::string detail = entry.detail;
  auto append_line = [&](std::string const& line) {
    if (!detail.empty()) {
      detail += "\n";
    }
    detail += line;
  };
  auto append_fact = [&](azzs::application::HistoryFactProjection const& fact) {
    auto line = fact.key + ": ";
    if (fact.disposition == HistoryFactDisposition::obtained) {
      line += fact.value;
    } else {
      line += winrt::to_string(resource_string(L"HistoryAndLogsNotObtained"));
      if (!fact.reason.empty()) {
        line += " (" + fact.reason + ")";
      }
    }
    append_line(line);
  };
  for (auto const& fact : entry.facts) {
    append_fact(fact);
  }
  for (auto const& timeline : entry.timeline) {
    auto line = std::string{azzs::application::to_string(timeline.kind)} +
                " | " + timeline.state;
    if (timeline.recorded_at_milliseconds.has_value()) {
      line += " | " + std::to_string(*timeline.recorded_at_milliseconds);
    }
    if (!timeline.detail.empty()) {
      line += " | " + timeline.detail;
    }
    append_line(line);
    for (auto const& fact : timeline.facts) {
      append_fact(fact);
    }
  }
  return detail;
}

}  // namespace

HistoryAndLogsPage::HistoryAndLogsPage() {
  InitializeComponent();
}

void HistoryAndLogsPage::bind(
    azzs::application::HistoryAndLogsService& service) {
  service_ = &service;
  project(service_->refresh());
}

void HistoryAndLogsPage::locate(std::string_view stable_id) {
  if (service_ != nullptr) {
    project(service_->locate(stable_id));
  }
}

void HistoryAndLogsPage::OnRefresh(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (service_ != nullptr) {
    project(service_->refresh(
        HistoryAndLogsFilter{.query = winrt::to_string(FilterTextBox().Text())}));
  }
}

void HistoryAndLogsPage::OnFilterChanged(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&) {
  if (service_ != nullptr) {
    project(service_->refresh(
        HistoryAndLogsFilter{.query = winrt::to_string(FilterTextBox().Text())}));
  }
}

void HistoryAndLogsPage::OnExportDiagnostic(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (service_ == nullptr) {
    return;
  }
  auto result = service_->export_diagnostic();
  project(result.snapshot);
  if (result.code != HistoryAndLogsActionCode::succeeded) {
    set_status(resource_string(L"HistoryAndLogsExportFailed"),
               InfoBarSeverity::Error);
    return;
  }
  if (!result.export_receipt.complete) {
    auto message =
        std::wstring{resource_string(L"HistoryAndLogsExportIncomplete")};
    replace_token(message, L"{file}",
                  std::wstring_view{
                      winrt::to_hstring(result.export_receipt.file_name).c_str()});
    replace_token(message, L"{count}",
                  std::to_wstring(result.export_receipt.missing_fact_count));
    set_status(winrt::hstring{message}, InfoBarSeverity::Warning);
    return;
  }
  auto message = std::wstring{resource_string(L"HistoryAndLogsExportSucceeded")};
  replace_token(message, L"{file}",
                std::wstring_view{winrt::to_hstring(result.export_receipt.file_name)
                                      .c_str()});
  set_status(winrt::hstring{message}, InfoBarSeverity::Success);
}

winrt::fire_and_forget HistoryAndLogsPage::OnClearLogs(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  try {
    auto lifetime = get_strong();
    if (service_ == nullptr || confirmation_dialog_open_) {
      co_return;
    }
    confirmation_dialog_open_ = true;
    ContentDialog dialog;
    dialog.XamlRoot(XamlRoot());
    dialog.Title(winrt::box_value(
        resource_string(L"HistoryAndLogsClearConfirmationTitle")));
    dialog.Content(winrt::box_value(
        resource_string(L"HistoryAndLogsClearConfirmationContent")));
    dialog.PrimaryButtonText(
        resource_string(L"HistoryAndLogsClearConfirmationConfirm"));
    dialog.CloseButtonText(
        resource_string(L"HistoryAndLogsClearConfirmationCancel"));
    if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) {
      confirmation_dialog_open_ = false;
      co_return;
    }
    confirmation_dialog_open_ = false;
    auto result = service_->clear_logs();
    project(result.snapshot);
    if (result.code == HistoryAndLogsActionCode::succeeded) {
      set_status(resource_string(L"HistoryAndLogsClearSucceeded"),
                 InfoBarSeverity::Success);
    } else if (result.snapshot.log.pending_clear.has_value()) {
      set_status(resource_string(L"HistoryAndLogsClearCompletionPending"),
                 InfoBarSeverity::Warning);
    } else {
      set_status(resource_string(L"HistoryAndLogsClearFailed"),
                 InfoBarSeverity::Error);
    }
  } catch (...) {
    confirmation_dialog_open_ = false;
    ::OutputDebugStringW(L"WinUI history clear dialog failed.\n");
  }
}

void HistoryAndLogsPage::project(HistoryAndLogsSnapshot const& snapshot) {
  HistoryItems().Children().Clear();
  LogItems().Children().Clear();
  for (auto const& entry : snapshot.history) {
    auto title = std::wstring{history_kind_text(entry.kind)};
    title += L" | ";
    title += winrt::to_hstring(entry.stable_id).c_str();
    auto state = std::wstring{resource_string(L"HistoryAndLogsStatePrefix")};
    state += winrt::to_hstring(entry.state).c_str();
    if (entry.retry) {
      state += L" | ";
      state += std::wstring{resource_string(L"HistoryAndLogsRetrySuffix")};
    }
    append_row(HistoryItems(), winrt::hstring{title}, winrt::hstring{state},
               winrt::to_hstring(history_detail_text(entry)));
  }
  HistoryEmptyText().Visibility(
      snapshot.history.empty() ? Microsoft::UI::Xaml::Visibility::Visible
                               : Microsoft::UI::Xaml::Visibility::Collapsed);

  if (!snapshot.log.available) {
    LogSummaryText().Text(resource_string(L"HistoryAndLogsLogUnavailable"));
  } else {
    auto summary = std::wstring{resource_string(
        snapshot.log.pending_clear.has_value()
            ? L"HistoryAndLogsClearPendingSummary"
            : L"HistoryAndLogsLogSummary")};
    if (snapshot.log.pending_clear.has_value()) {
      replace_token(
          summary, L"{segment}",
          std::to_wstring(snapshot.log.pending_clear->cutoff_segment));
      replace_token(
          summary, L"{sequence}",
          std::to_wstring(snapshot.log.pending_clear->cutoff_sequence));
    } else {
      replace_token(summary, L"{segment}",
                    std::to_wstring(snapshot.log.active_segment));
      replace_token(summary, L"{sequence}",
                    std::to_wstring(snapshot.log.last_sequence));
    }
    LogSummaryText().Text(winrt::hstring{summary});
  }
  auto preview = std::wstring{resource_string(L"HistoryAndLogsDiagnosticPreview")};
  replace_token(preview, L"{start}",
                coverage_time_text(snapshot.log.coverage_started_at));
  replace_token(preview, L"{end}",
                coverage_time_text(snapshot.log.coverage_ended_at));
  replace_token(preview, L"{bytes}",
                std::to_wstring(snapshot.log.durable_bytes));
  replace_token(preview, L"{gaps}",
                std::to_wstring(snapshot.log.coverage_gap_count));
  preview += L"\n";
  preview += std::wstring{resource_string(L"HistoryAndLogsDebugStatus")};
  auto const debug_context =
      azzs::application::make_debug_log_policy_context(snapshot.debug);
  replace_token(preview, L"{state}",
                debug_context.facts_available
                    ? (debug_context.debug_mode == "enabled"
                           ? std::wstring{resource_string(
                                 L"HistoryAndLogsDebugEnabled")}
                           : std::wstring{resource_string(
                                 L"HistoryAndLogsDebugDisabled")})
                    : std::wstring{resource_string(
                          L"HistoryAndLogsNotObtained")});
  replace_token(preview, L"{granularity}",
                !debug_context.facts_available ||
                        debug_context.granularity.empty()
                    ? std::wstring{resource_string(L"HistoryAndLogsNotObtained")}
                    : std::wstring{winrt::to_hstring(debug_context.granularity)
                                       .c_str()});
  if (!debug_context.facts_available &&
      !debug_context.not_obtained_reason.empty()) {
    preview += L"\n";
    preview += std::wstring{resource_string(L"HistoryAndLogsNotObtained")};
    preview += L": ";
    preview += winrt::to_hstring(debug_context.not_obtained_reason).c_str();
  }
  DiagnosticPreviewText().Text(winrt::hstring{preview});
  AutomationProperties::SetName(DiagnosticPreviewText(), winrt::hstring{preview});
  for (auto const& event : snapshot.log.events) {
    auto title = std::wstring{event_kind_text(event.kind)};
    title += L" | ";
    title += winrt::to_hstring(event.component).c_str();
    title += L" / ";
    title += winrt::to_hstring(event.stage).c_str();
    auto state = std::wstring{result_text(event.result)};
    state += L" | ";
    state += std::wstring{resource_string(L"HistoryAndLogsSegmentLabel")};
    state += L" ";
    state += std::to_wstring(event.segment);
    state += L" | ";
    state += std::wstring{resource_string(L"HistoryAndLogsEventLabel")};
    state += L" ";
    state += std::to_wstring(event.sequence);
    std::string detail;
    if (event.error.has_value()) {
      detail = event.error->source;
      if (!detail.empty() || event.error->code != 0) {
        if (!detail.empty()) {
          detail += ": ";
        }
        detail += std::to_string(event.error->code);
      }
      if (!event.error->message.empty()) {
        if (!detail.empty()) {
          detail += " | ";
        }
        detail += event.error->message;
      }
    }
    if (event.coverage_gap.has_value()) {
      if (!detail.empty()) {
        detail += " | ";
      }
      detail += event.coverage_gap->reason;
    }
    for (auto const& field : event.fields) {
      if (!detail.empty()) {
        detail += " | ";
      }
      detail += field.key;
      detail += ": ";
      detail += field.value;
    }
    append_row(LogItems(), winrt::hstring{title}, winrt::hstring{state},
               winrt::to_hstring(detail));
  }
  LogEmptyText().Visibility(
      snapshot.log.available && !snapshot.log.pending_clear.has_value() &&
              snapshot.log.events.empty()
          ? Microsoft::UI::Xaml::Visibility::Visible
          : Microsoft::UI::Xaml::Visibility::Collapsed);
  if (!snapshot.log.available) {
    set_status(resource_string(L"HistoryAndLogsLogUnavailable"),
               InfoBarSeverity::Warning);
  } else if (snapshot.log.pending_clear.has_value()) {
    set_status(resource_string(L"HistoryAndLogsClearCompletionPending"),
               InfoBarSeverity::Warning);
  } else {
    StatusInfoBar().IsOpen(false);
  }
}

void HistoryAndLogsPage::set_status(
    winrt::hstring const& message,
    Microsoft::UI::Xaml::Controls::InfoBarSeverity severity) {
  StatusInfoBar().Severity(severity);
  StatusInfoBar().Message(message);
  StatusInfoBar().IsOpen(true);
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
