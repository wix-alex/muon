// Copyright (c) 2015 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/atom_download_manager_delegate.h"

#include <string>
#include <utility>
#include <vector>

#include "atom/browser/native_window.h"
#include "base/bind.h"
#include "base/files/file_util.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/download_completion_blocker.h"
#include "chrome/browser/download/download_item_model.h"
#include "chrome/browser/extensions/api/file_system/file_entry_picker.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/safe_browsing/safe_browsing_service.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/safe_browsing/file_type_policies.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/download_danger_type.h"
#include "content/public/browser/download_manager.h"
#include "net/base/filename_util.h"
#include "net/base/mime_util.h"
#include "vendor/brightray/browser/inspectable_web_contents.h"

namespace atom {

namespace {

using content::BrowserThread;
using content::DownloadItem;
using content::DownloadManager;
using safe_browsing::DownloadFileType;
using safe_browsing::DownloadProtectionService;

#if defined(FULL_SAFE_BROWSING)

const char kSafeBrowsingUserDataKey[] = "Safe Browsing ID";

void CheckDownloadUrlDone(
    const DownloadTargetDeterminerDelegate::CheckDownloadUrlCallback& callback,
    safe_browsing::DownloadCheckResult result) {
  if (result == safe_browsing::DownloadCheckResult::SAFE ||
      result == safe_browsing::DownloadCheckResult::UNKNOWN) {
    callback.Run(content::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS);
  } else {
    callback.Run(content::DOWNLOAD_DANGER_TYPE_DANGEROUS_URL);
  }
}
#endif  // FULL_SAFE_BROWSING

const DownloadPathReservationTracker::FilenameConflictAction
    kDefaultPlatformConflictAction = DownloadPathReservationTracker::UNIQUIFY;

}  // namespace

bool AtomDownloadManagerDelegate::IsDownloadReadyForCompletion(
    DownloadItem* item,
    const base::Closure& internal_complete_callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
#if defined(FULL_SAFE_BROWSING)
  DownloadCompletionBlocker* state = static_cast<DownloadCompletionBlocker*>(
      item->GetUserData(&kSafeBrowsingUserDataKey));
  if (!state) {
    // Begin the safe browsing download protection check.
    DownloadProtectionService* service = GetDownloadProtectionService();
    if (service) {
      state = new DownloadCompletionBlocker();
      state->set_callback(internal_complete_callback);
      item->SetUserData(&kSafeBrowsingUserDataKey, base::WrapUnique(state));
      service->CheckClientDownload(
          item,
          base::Bind(&AtomDownloadManagerDelegate::CheckClientDownloadDone,
                     weak_ptr_factory_.GetWeakPtr(),
                     item->GetId()));
      return false;
    }

    // In case the service was disabled between the download starting and now,
    // we need to restore the danger state.
    content::DownloadDangerType danger_type = item->GetDangerType();
    if (DownloadItemModel(item).GetDangerLevel() !=
            DownloadFileType::NOT_DANGEROUS &&
        (danger_type == content::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS ||
         danger_type ==
             content::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT)) {
      item->OnContentCheckCompleted(
            content::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE,
            content::DOWNLOAD_INTERRUPT_REASON_FILE_BLOCKED);

      content::BrowserThread::PostTask(content::BrowserThread::UI, FROM_HERE,
                                       internal_complete_callback);
      return false;
    }
  } else if (!state->is_complete()) {
    state->set_callback(internal_complete_callback);
    return false;
  }

#endif
  return true;
}

bool AtomDownloadManagerDelegate::GenerateFileHash() {
#if defined(FULL_SAFE_BROWSING)
  Profile* profile = static_cast<Profile*>(
    download_manager_->GetBrowserContext());
  return profile->GetPrefs()->GetBoolean(prefs::kSafeBrowsingEnabled)
    && g_browser_process->safe_browsing_service()->DownloadBinHashNeeded();
#else
  return false;
#endif
}

DownloadProtectionService*
    AtomDownloadManagerDelegate::GetDownloadProtectionService() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
#if defined(FULL_SAFE_BROWSING)
  Profile* profile = static_cast<Profile*>(
    download_manager_->GetBrowserContext());

  safe_browsing::SafeBrowsingService* sb_service =
      g_browser_process->safe_browsing_service();
  if (sb_service && sb_service->download_protection_service() &&
      profile->GetPrefs()->GetBoolean(prefs::kSafeBrowsingEnabled)) {
    return sb_service->download_protection_service();
  }
#endif
  return NULL;
}

void AtomDownloadManagerDelegate::ShouldCompleteDownloadInternal(
    uint32_t download_id,
    const base::Closure& user_complete_callback) {
  DownloadItem* item = download_manager_->GetDownload(download_id);
  if (!item)
    return;
  if (ShouldCompleteDownload(item, user_complete_callback))
    user_complete_callback.Run();
}

bool AtomDownloadManagerDelegate::ShouldCompleteDownload(
    DownloadItem* item,
    const base::Closure& user_complete_callback) {
  return IsDownloadReadyForCompletion(item, base::Bind(
      &AtomDownloadManagerDelegate::ShouldCompleteDownloadInternal,
      weak_ptr_factory_.GetWeakPtr(), item->GetId(), user_complete_callback));
}

void AtomDownloadManagerDelegate::CheckDownloadUrl(
    DownloadItem* download,
    const base::FilePath& suggested_path,
    const CheckDownloadUrlCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

#if defined(FULL_SAFE_BROWSING)
  safe_browsing::DownloadProtectionService* service =
      GetDownloadProtectionService();
  if (service) {
    service->CheckDownloadUrl(download,
                              base::Bind(&CheckDownloadUrlDone, callback));
    return;
  }
#endif
  callback.Run(content::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS);
}

#if defined(FULL_SAFE_BROWSING)
void AtomDownloadManagerDelegate::CheckClientDownloadDone(
    uint32_t download_id,
    safe_browsing::DownloadCheckResult result) {
  DownloadItem* item = download_manager_->GetDownload(download_id);
  if (!item || (item->GetState() != DownloadItem::IN_PROGRESS))
    return;

  if (item->GetDangerType() == content::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS ||
      item->GetDangerType() ==
      content::DOWNLOAD_DANGER_TYPE_MAYBE_DANGEROUS_CONTENT) {
    content::DownloadDangerType danger_type =
        content::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS;

    switch (result) {
      case safe_browsing::DownloadCheckResult::UNKNOWN:
        if (DownloadItemModel(item).GetDangerLevel() !=
            DownloadFileType::NOT_DANGEROUS) {
          danger_type = content::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE;
        }
        break;
      case safe_browsing::DownloadCheckResult::SAFE:
        if (DownloadItemModel(item).GetDangerLevel() ==
            DownloadFileType::DANGEROUS) {
          danger_type = content::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE;
        }
        break;
      case safe_browsing::DownloadCheckResult::DANGEROUS:
        danger_type = content::DOWNLOAD_DANGER_TYPE_DANGEROUS_CONTENT;
        break;
      case safe_browsing::DownloadCheckResult::UNCOMMON:
        danger_type = content::DOWNLOAD_DANGER_TYPE_UNCOMMON_CONTENT;
        break;
      case safe_browsing::DownloadCheckResult::DANGEROUS_HOST:
        danger_type = content::DOWNLOAD_DANGER_TYPE_DANGEROUS_HOST;
        break;
      case safe_browsing::DownloadCheckResult::POTENTIALLY_UNWANTED:
        danger_type = content::DOWNLOAD_DANGER_TYPE_POTENTIALLY_UNWANTED;
        break;
    }

    if (danger_type != content::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS) {
      item->OnContentCheckCompleted(
          danger_type,
          content::DOWNLOAD_INTERRUPT_REASON_FILE_BLOCKED);
    }
  }

  DownloadCompletionBlocker* state = static_cast<DownloadCompletionBlocker*>(
      item->GetUserData(&kSafeBrowsingUserDataKey));
  state->CompleteDownload();
}
#endif  // FULL_SAFE_BROWSING

AtomDownloadManagerDelegate::AtomDownloadManagerDelegate(
    content::DownloadManager* manager)
    : download_manager_(manager),
      download_prefs_(new DownloadPrefs(
         static_cast<Profile*>(manager->GetBrowserContext()))),
      weak_ptr_factory_(this) {}

AtomDownloadManagerDelegate::~AtomDownloadManagerDelegate() {
  if (download_manager_) {
    DCHECK_EQ(static_cast<content::DownloadManagerDelegate*>(this),
              download_manager_->GetDelegate());
    download_manager_->SetDelegate(nullptr);
    download_manager_ = nullptr;
  }
}

void AtomDownloadManagerDelegate::GetItemSavePath(DownloadItem* item,
                                                  base::FilePath* path) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Locker locker(isolate);
  v8::HandleScope handle_scope(isolate);
  api::DownloadItem* download = api::DownloadItem::FromWrappedClass(isolate,
                                                                    item);
  if (download && !download->GetSavePath().empty())
    *path = download->GetSavePath();
}

bool AtomDownloadManagerDelegate::GetExtension(
    content::DownloadItem* item,
    const base::FilePath& target_path,
    base::FilePath::StringType* extension) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Locker locker(isolate);
  v8::HandleScope handle_scope(isolate);
  api::DownloadItem* download = api::DownloadItem::FromWrappedClass(isolate,
                                                                    item);
  if (download && !download->GetMimeType().empty()) {
    if (net::GetPreferredExtensionForMimeType(download->GetMimeType(),
                                              extension))
      return true;
  }

  base::FilePath::StringType target_extension = target_path.Extension();
  if (!target_extension.empty()) {
    target_extension.erase(target_extension.begin());  // Erase preceding '.'.
    *extension = target_extension;
    return true;
  }
  return false;
}

void AtomDownloadManagerDelegate:: OnDownloadItemSelected(
    const content::DownloadTargetCallback& callback,
    std::unique_ptr<DownloadTargetInfo> target_info,
    api::DownloadItem* download_item,
    const std::vector<base::FilePath>& paths) {
  DCHECK(!paths.empty());
  // Remember the last selected download directory.
  Profile* profile = static_cast<Profile*>(
      download_manager_->GetBrowserContext());
  profile->GetPrefs()->SetFilePath(prefs::kDownloadDefaultDirectory,
                                   paths[0].DirName());
  if (download_item)
    download_item->SetSavePath(paths[0]);

  callback.Run(paths[0],
               target_info->target_disposition,
               target_info->danger_type, paths[0],
               target_info->result);
}

void AtomDownloadManagerDelegate::OnDownloadItemSelectionCancelled(
    const content::DownloadTargetCallback& callback,
    DownloadItem* item) {
  item->Remove();
  base::FilePath path;
  callback.Run(path,
               DownloadItem::TARGET_DISPOSITION_PROMPT,
               content::DOWNLOAD_DANGER_TYPE_NOT_DANGEROUS, path,
               content::DOWNLOAD_INTERRUPT_REASON_USER_CANCELED);
}

void AtomDownloadManagerDelegate::DetermineLocalPath(
    DownloadItem* download,
    const base::FilePath& virtual_path,
    const DownloadTargetDeterminerDelegate::LocalPathCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  callback.Run(virtual_path);
}

void AtomDownloadManagerDelegate::OnDownloadTargetDetermined(
    int32_t download_id,
    const content::DownloadTargetCallback& callback,
    std::unique_ptr<DownloadTargetInfo> target_info) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto item = download_manager_->GetDownload(download_id);
  if (!item)
    return;

  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Locker locker(isolate);
  v8::HandleScope handle_scope(isolate);
  api::DownloadItem* download_item = api::DownloadItem::FromWrappedClass(
      isolate, item);

  if (!download_item)
    download_item = atom::api::DownloadItem::Create(isolate, item).get();

  base::FilePath path = target_info->target_path;

  NativeWindow* window = nullptr;
  content::WebContents* web_contents = item->GetWebContents();
  auto relay = web_contents ? NativeWindowRelay::FromWebContents(web_contents)
                            : nullptr;
  if (relay)
    window = relay->window.get();

  GetItemSavePath(item, &path);

  // Show save dialog if save path was not set already on item
  ui::SelectFileDialog::FileTypeInfo file_type_info;
  std::vector<base::FilePath::StringType> extensions;
  base::FilePath::StringType extension;

  if (GetExtension(item, path, &extension)) {
    extensions.push_back(extension);
    file_type_info.extensions.push_back(extensions);
  }
  file_type_info.include_all_files = true;

  new extensions::FileEntryPicker(
      window->inspectable_web_contents()->GetWebContents(),
      path,
      file_type_info,
      ui::SelectFileDialog::SELECT_SAVEAS_FILE,
      base::Bind(&AtomDownloadManagerDelegate::OnDownloadItemSelected,
                 base::Unretained(this), callback,
                 base::Passed(std::move(target_info)), download_item),
      base::Bind(&AtomDownloadManagerDelegate::OnDownloadItemSelectionCancelled,
                 base::Unretained(this), callback, item));
}

void AtomDownloadManagerDelegate::Shutdown() {
  weak_ptr_factory_.InvalidateWeakPtrs();
  download_manager_ = nullptr;
}

bool AtomDownloadManagerDelegate::DetermineDownloadTarget(
    DownloadItem* download,
    const content::DownloadTargetCallback& callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  Profile* browser_context = static_cast<Profile*>(
      download_manager_->GetBrowserContext());
  base::FilePath default_download_path(browser_context->GetPrefs()->GetFilePath(
      prefs::kDownloadDefaultDirectory));

  base::FilePath virtual_path = download->GetForcedFilePath();

  if (virtual_path.empty()) {
    std::string suggested_filename(download->GetSuggestedFilename());
    if (suggested_filename.empty() &&
        download->GetMimeType() == "application/x-x509-user-cert") {
      suggested_filename = "user.crt";
    }

    base::FilePath generated_filename = net::GenerateFileName(
        download->GetURL(),
        download->GetContentDisposition(),
        std::string(),
        suggested_filename,
        download->GetMimeType(),
        std::string());

    virtual_path = default_download_path.Append(generated_filename);
  }

  DownloadTargetDeterminer::CompletionCallback target_determined_callback =
      base::Bind(&AtomDownloadManagerDelegate::OnDownloadTargetDetermined,
                 weak_ptr_factory_.GetWeakPtr(),
                 download->GetId(),
                 callback);

  DownloadTargetDeterminer::Start(
      download,
      virtual_path,
      kDefaultPlatformConflictAction, download_prefs_.get(), this,
      target_determined_callback);

  return true;
}

void AtomDownloadManagerDelegate::ReserveVirtualPath(
    DownloadItem* download,
    const base::FilePath& virtual_path,
    bool create_directory,
    DownloadPathReservationTracker::FilenameConflictAction conflict_action,
    const ReservedPathCallback& callback) {
      DownloadPathReservationTracker::GetReservedPath(
          download,
          virtual_path,
          download_prefs_->DownloadPath(),
          true,
          conflict_action,
          callback);
}

bool AtomDownloadManagerDelegate::ShouldOpenDownload(
    DownloadItem* download,
    const content::DownloadOpenDelayedCallback& callback) {
  return true;
}

void AtomDownloadManagerDelegate::GetNextId(
    const content::DownloadIdCallback& callback) {
  static uint32_t next_id = DownloadItem::kInvalidId + 1;
  callback.Run(next_id++);
}

}  // namespace atom
