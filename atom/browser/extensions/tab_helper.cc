// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/extensions/tab_helper.h"

#include <map>
#include <utility>
#include "atom/browser/extensions/api/atom_extensions_api_client.h"
#include "atom/browser/extensions/atom_extension_web_contents_observer.h"
#include "atom/browser/native_window.h"
#include "atom/common/native_mate_converters/callback.h"
#include "atom/common/native_mate_converters/gurl_converter.h"
#include "atom/common/native_mate_converters/value_converter.h"
#include "base/strings/utf_string_conversions.h"
#include "brave/browser/brave_browser_context.h"
#include "brave/browser/guest_view/tab_view/tab_view_guest.h"
#include "brave/browser/resource_coordinator/guest_tab_manager.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/resource_coordinator/tab_lifecycle_unit_external.h"
#include "chrome/browser/sessions/session_tab_helper.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/tab_contents/tab_contents_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_order_controller.h"
#include "components/sessions/core/session_id.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/component_extension_resource_manager.h"
#include "extensions/browser/extension_api_frame_id_map.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extensions_browser_client.h"
#include "extensions/browser/file_reader.h"
#include "extensions/common/extension_messages.h"
#include "native_mate/arguments.h"
#include "native_mate/dictionary.h"
#include "net/base/filename_util.h"
#include "ui/base/resource/resource_bundle.h"

using brave::BraveBrowserContext;
using guest_view::GuestViewManager;
using resource_coordinator::TabManager;

DEFINE_WEB_CONTENTS_USER_DATA_KEY(extensions::TabHelper);

namespace keys {
const char kIdKey[] = "id";
const char kActiveKey[] = "active";
const char kIncognitoKey[] = "incognito";
const char kWindowIdKey[] = "windowId";
const char kTitleKey[] = "title";
const char kUrlKey[] = "url";
const char kStatusKey[] = "status";
const char kAudibleKey[] = "audible";
const char kDiscardedKey[] = "discarded";
const char kAutoDiscardableKey[] = "autoDiscardable";
const char kHighlightedKey[] = "highlighted";
const char kIndexKey[] = "index";
const char kPinnedKey[] = "pinned";
const char kSelectedKey[] = "selected";
}  // namespace keys

static std::map<int32_t, std::pair<int, int>> render_view_map_;

namespace extensions {

namespace {

TabManager* GetTabManager() {
  return g_browser_process->GetTabManager();
}

}  // namespace

TabHelper::TabHelper(content::WebContents* contents)
    : content::WebContentsObserver(contents),
      values_(new base::DictionaryValue),
      script_executor_(
          new ScriptExecutor(contents, &script_execution_observers_)),
      index_(TabStripModel::kNoTab),
      pinned_(false),
      discarded_(false),
      auto_discardable_(true),
      active_(false),
      is_placeholder_(false),
      window_closing_(false),
      opener_tab_id_(TabStripModel::kNoTab),
      browser_(nullptr) {
  SessionTabHelper::CreateForWebContents(contents);
  SetWindowId(-1);

  RenderViewCreated(contents->GetRenderViewHost());
  contents->ForEachFrame(
      base::Bind(&TabHelper::SetTabId, base::Unretained(this)));

  AtomExtensionWebContentsObserver::CreateForWebContents(contents);
  BrowserList::AddObserver(this);
}

TabHelper::~TabHelper() {
  BrowserList::RemoveObserver(this);
}

// static
void TabHelper::CreateTab(content::WebContents* owner,
                content::BrowserContext* browser_context,
                const base::DictionaryValue& create_params,
                const GuestViewManager::WebContentsCreatedCallback& callback) {
  BraveBrowserContext* profile =
      BraveBrowserContext::FromBrowserContext(browser_context);
  auto guest_view_manager = static_cast<GuestViewManager*>(
      profile->GetGuestManager());
  DCHECK(guest_view_manager);

  std::unique_ptr<base::DictionaryValue> params =
      create_params.CreateDeepCopy();

  params->SetString("partition",
      profile->partition_with_prefix());

  if (profile->HasParentContext()) {
    params->SetString("parent_partition",
        profile->original_context()->partition_with_prefix());
  }

  guest_view_manager->CreateGuest(brave::TabViewGuest::Type,
                                  owner,
                                  *params.get(),
                                  callback);
}

// static
content::WebContents* TabHelper::CreateTab(content::WebContents* owner,
                            content::WebContents::CreateParams create_params) {
  auto guest_view_manager = static_cast<GuestViewManager*>(
      create_params.browser_context->GetGuestManager());
  DCHECK(guest_view_manager);

  return guest_view_manager->CreateGuestWithWebContentsParams(
      brave::TabViewGuest::Type,
      owner,
      create_params);
}

// static
void TabHelper::DestroyTab(content::WebContents* tab) {
  auto guest = brave::TabViewGuest::FromWebContents(tab);
  DCHECK(guest);
  guest->Destroy(true);
}

// static
int TabHelper::GetTabStripIndex(int window_id, int index) {
  auto& all_tabs = AllTabContentses();
  for (auto it = all_tabs.begin(), end = all_tabs.end(); it != end; ++it) {
    auto tab_helper = FromWebContents(*it);
    if (tab_helper &&
        tab_helper->get_index() == index &&
        tab_helper->window_id() == window_id)
      return tab_helper->get_index();
  }
  return TabStripModel::kNoTab;
}

bool TabHelper::AttachGuest(int window_id, int index) {
  DCHECK(!guest()->attached());

  for (auto* browser : *BrowserList::GetInstance()) {
    if (browser->session_id().id() == window_id) {
      index_ = index;
      browser->tab_strip_model()->ReplaceWebContentsAt(
          GetTabStripIndex(window_id, index_),
          base::WrapUnique(web_contents())).release();
      return true;
    }
  }
  return false;
}

content::WebContents* TabHelper::DetachGuest() {
  if (guest()->attached()) {
    // create temporary null placeholder
    content::WebContents::CreateParams
        create_params(browser_->tab_strip_model()->profile());
    auto null_contents = GetTabManager()->CreateNullContents(
        create_params, web_contents());

    null_contents->GetController().CopyStateFrom(
        web_contents()->GetController(), false);

    auto null_helper = FromWebContents(null_contents.get());
    null_helper->index_ = get_index();
    null_helper->pinned_ = pinned_;
    // transfer window closing state
    null_helper->window_closing_ = window_closing_;
    window_closing_ = false;

    null_helper->SetPlaceholder(true);

    // Replace the detached tab with the null placeholder
    browser_->tab_strip_model()->ReplaceWebContentsAt(
        get_index(), std::move(null_contents)).release();

    return null_contents.get();
  }
  return nullptr;
}

void TabHelper::DidAttach() {
  MaybeRequestWindowClose();

  if (is_placeholder()) {
    guest()->SetCanRunInDetachedState(false);
    if (!pinned_ && !IsDiscarded()) {
      // This is a placeholder that was used for a tab move so get rid of it
      base::ThreadTaskRunnerHandle::Get()->PostTask(FROM_HERE,
        base::Bind(TabHelper::DestroyTab,
            base::Unretained(web_contents())));
    } else {
      MaybeAttachOrCreatePinnedTab();
    }
  }
}

void TabHelper::SetPlaceholder(bool is_placeholder) {
  is_placeholder_  = is_placeholder;
  if (!is_placeholder_)
    // set to false in DidAttach to prevent early destruction
    guest()->SetCanRunInDetachedState(true);
}

void TabHelper::MaybeRequestWindowClose() {
  if (window_closing_ && browser()) {
    auto window = static_cast<atom::NativeWindow*>(browser_->window());
    window->RequestToClosePage();
  }
}

void TabHelper::WillCloseWindow(bool* prevent_default) {
  window_closing_ = false;

  if (browser() &&
      is_pinned() &&
      !is_placeholder() &&
      !browser_shutdown::IsTryingToQuit() &&
      BrowserList::GetInstance()->size() > 1) {
    // TODO(bridiver) - enable pinned tab transfer
    // *prevent_default = true;
    // window_closing_ = true;
    browser()->window()->Deactivate();
    browser()->window()->Hide();
  }
}

void TabHelper::OnBrowserRemoved(Browser* browser) {
  MaybeRequestWindowClose();

  if (browser_ != nullptr && browser_ == browser) {
    auto window = static_cast<atom::NativeWindow*>(browser_->window());
    window->RemoveObserver(this);

    browser_->tab_strip_model()->RemoveObserver(this);
    browser_ = nullptr;
    index_ = TabStripModel::kNoTab;
  }
}

void TabHelper::OnBrowserSetLastActive(Browser* browser) {
  MaybeRequestWindowClose();
  MaybeAttachOrCreatePinnedTab();
}

void TabHelper::MaybeAttachOrCreatePinnedTab() {
  if (window_closing_ ||
      !is_pinned() ||
      !is_placeholder() ||
      !guest()->attached() ||
      browser_ != BrowserList::GetInstance()->GetLastActive())
    return;

  // TODO(bridiver) - enable pinned tab transfer
  // content::WebContents* pinned_web_contents = nullptr;
  // for (auto* browser : *BrowserList::GetInstance()) {
  //   auto web_contents =
  //       browser->tab_strip_model()->GetWebContentsAt(get_index());
  //   if (web_contents) {
  //     auto tab_helper = FromWebContents(web_contents);
  //     if (!tab_helper->is_placeholder()) {
  //       pinned_web_contents = web_contents;
  //       tab_helper->DetachGuest();
  //       break;
  //     }
  //   }
  // }

  // if (pinned_web_contents) {
  //   browser_->tab_strip_model()->ReplaceWebContentsAt(get_index(),
  //                                                     pinned_web_contents);
  // } else {
    SetPlaceholder(false);
    web_contents()->NavigatedByUser();
    guest()->Load();
  // }
}

void TabHelper::TabReplacedAt(TabStripModel* tab_strip_model,
                               content::WebContents* old_contents,
                               content::WebContents* new_contents,
                               int index) {
  if (old_contents != web_contents())
    return;

  auto old_browser = browser_;

  brave::TabViewGuest* old_guest = guest();
  int guest_instance_id = old_guest->guest_instance_id();

  auto new_helper = FromWebContents(new_contents);
  new_helper->index_ = get_index();
  new_helper->pinned_ = pinned_;
  new_helper->opener_tab_id_ = opener_tab_id_;

  OnBrowserRemoved(old_browser);
  new_helper->UpdateBrowser(old_browser);

  brave::TabViewGuest* new_guest = new_helper->guest();
  old_contents->WasHidden();

  const base::DictionaryValue* attach_params =
      old_guest->attach_params()->CreateDeepCopy().release();
  new_guest->SetAttachParams(*attach_params);
  new_guest->TabIdChanged();
}

void TabHelper::TabDetachedAt(content::WebContents* contents,
                              int index,
                              bool was_active) {
  if (contents != web_contents())
    return;

  OnBrowserRemoved(browser_);
}

void TabHelper::TabPinnedStateChanged(TabStripModel* tab_strip_model,
                             content::WebContents* contents,
                             int index) {
  if (contents != web_contents())
    return;

  MaybeAttachOrCreatePinnedTab();
}

void TabHelper::ActiveTabChanged(content::WebContents* old_contents,
                                content::WebContents* new_contents,
                                int index,
                                int reason) {
  if (old_contents == web_contents()) {
    active_ = false;
  }

  if (new_contents == web_contents()) {
    active_ = true;
  }
}

void TabHelper::SetActive(bool active) {
  if (active) {
    if (browser_) {
      browser_->tab_strip_model()->ActivateTabAt(get_index(), true);
      if (!IsDiscarded()) {
        web_contents()->WasShown();
      }
    } else {
      active_ = true;
    }

    MaybeAttachOrCreatePinnedTab();
  } else {
    active_ = false;
    web_contents()->WasHidden();
  }
}

void TabHelper::OnVisibilityChanged(content::Visibility visibility) {
  auto helper = content::RestoreHelper::FromWebContents(web_contents());
  if (visibility == content::Visibility::VISIBLE && helper) {
    // load the tab if it is shown without being activate (tab preview)
    discarded_ = false;
    if (helper) {
      helper->RemoveRestoreHelper();
    }

    web_contents()->GetController().Reload(content::ReloadType::NORMAL, true);
  }
}

void TabHelper::UpdateBrowser(Browser* browser) {
  browser_ = browser;
  browser_->tab_strip_model()->AddObserver(this);
  SetWindowId(browser_->session_id().id());
  static_cast<atom::NativeWindow*>(browser_->window())->AddObserver(this);

  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_TAB_PARENTED,
      content::Source<content::WebContents>(web_contents()),
      content::NotificationService::NoDetails());
}

void TabHelper::SetBrowser(Browser* browser) {
  if (browser == browser_)
    return;

  // hack for browserAction
  if (get_index() == -2) {
    return;
  }

  if (browser_) {
    if (get_index() != TabStripModel::kNoTab)
      // TODO(hferreiro)
      browser_->tab_strip_model()->DetachWebContentsAt(get_index()).release();

    OnBrowserRemoved(browser_);
  }

  if (browser) {
    UpdateBrowser(browser);
    if (index_ < TabStripModel::kNoTab) {
      // hack for browserAction
      // TODO(bridiver) - use extension view
      return;
    }

    ui::PageTransition transition_type = ui::PAGE_TRANSITION_TYPED;

    int add_types = TabStripModel::ADD_NONE;
    add_types |= active_ ? TabStripModel::ADD_ACTIVE : 0;
    if (opener_tab_id_ != TabStripModel::kNoTab) {
      add_types |= TabStripModel::ADD_INHERIT_OPENER;
      transition_type = ui::PAGE_TRANSITION_LINK;
    }

    browser_->tab_strip_model()->AddWebContents(
        base::WrapUnique(web_contents()), index_, transition_type, add_types);
  } else {
    browser_ = nullptr;
  }
}

void TabHelper::TabInsertedAt(TabStripModel* tab_strip_model,
                     content::WebContents* contents,
                     int index,
                     bool active) {
  if (contents != web_contents())
    return;

  if (discarded_) {
    resource_coordinator::TabLifecycleUnitExternal::FromWebContents(
        web_contents())
        ->SetAutoDiscardable(false);
  }

  guest()->Load();
}

void TabHelper::SetWindowId(const int32_t& id) {
  if (SessionTabHelper::FromWebContents(web_contents())->window_id().id() == id)
    return;

  SessionID session = SessionID::FromSerializedValue(id);
  SessionTabHelper::FromWebContents(web_contents())->SetWindowID(session);
}

int32_t TabHelper::window_id() const {
  return SessionTabHelper::FromWebContents(web_contents())->window_id().id();
}

void TabHelper::SetAutoDiscardable(bool auto_discardable) {
  auto_discardable_ = auto_discardable;
  auto* tab_lifecycle_unit_external =
      resource_coordinator::TabLifecycleUnitExternal::FromWebContents(
          web_contents());
  if (tab_lifecycle_unit_external) {
    tab_lifecycle_unit_external->SetAutoDiscardable(auto_discardable);
  }
}

bool TabHelper::Discard() {
  if (IsDiscarded())
    return false;

  if (!resource_coordinator::TabLifecycleUnitExternal::FromWebContents(
      web_contents())) {
    discarded_ = true;
    content::RestoreHelper::CreateForWebContents(web_contents());
    auto helper = content::RestoreHelper::FromWebContents(web_contents());
    helper->ClearNeedsReload();
    return true;
  } else {
    return resource_coordinator::TabLifecycleUnitExternal::FromWebContents(
        web_contents())->DiscardTab();
  }
}

bool TabHelper::IsDiscarded() {
  if (discarded_) {
    return true;
  }
  auto* tab_lifecycle_unit_external =
      resource_coordinator::TabLifecycleUnitExternal::FromWebContents(
          web_contents());
  return tab_lifecycle_unit_external &&
         tab_lifecycle_unit_external->IsDiscarded();
}

void TabHelper::SetPinned(bool pinned) {
  if (pinned == pinned_)
    return;

  pinned_ = pinned;
  if (browser()) {
    browser()->tab_strip_model()->SetTabPinned(get_index(), pinned);
  }

  if (pinned_) {
    SetPlaceholder(true);
  } else {
    SetPlaceholder(false);
  }
}

bool TabHelper::IsPinned() const {
  return pinned_;
}

bool TabHelper::MoveTo(int index, int window_id, bool foreground) {
  for (auto* b : *BrowserList::GetInstance()) {
    if (b->session_id().id() == window_id) {
      if (get_index() != TabStripModel::kNoTab)
        browser()->tab_strip_model()->DetachWebContentsAt(get_index()).release();

      UpdateBrowser(b);

      if (index != TabStripModel::kNoTab) {
        int add_types = TabStripModel::ADD_NONE;
        add_types |= foreground ? TabStripModel::ADD_ACTIVE : 0;
        b->tab_strip_model()->InsertWebContentsAt(
            index, base::WrapUnique(web_contents()), add_types);
      } else {
        b->tab_strip_model()->AppendWebContents(
            base::WrapUnique(web_contents()), foreground);
      }
      return true;
    }
  }
  return false;
}

void TabHelper::SetTabIndex(int index) {
  index_ = index;
  if (browser()) {
    browser()->tab_strip_model()->MoveWebContentsAt(
        get_index(), index, false);
  }
}

bool TabHelper::is_active() const {
  if (browser()) {
    return browser()->tab_strip_model()->GetActiveWebContents() ==
        web_contents();
  } else {
    return active_;
  }
}

brave::TabViewGuest* TabHelper::guest() const {
  auto guest = brave::TabViewGuest::FromWebContents(web_contents());
  DCHECK(guest);
  return guest;
}

void TabHelper::SetTabValues(const base::DictionaryValue& values) {
  values_->MergeDictionary(&values);
}

void TabHelper::SetOpener(int opener_tab_id) {
  opener_tab_id_ = opener_tab_id;
}

void TabHelper::RenderViewCreated(content::RenderViewHost* render_view_host) {
  if (!render_view_host)
    return;

  // only allow one call to create
  auto search = render_view_map_.find(session_id());
  if (search != render_view_map_.end())
    return;

  render_view_map_[session_id()] = std::make_pair(
      render_view_host->GetProcess()->GetID(),
      render_view_host->GetRoutingID());
}

void TabHelper::RenderViewHostChanged(content::RenderViewHost* old_host,
                             content::RenderViewHost* new_host) {
  render_view_map_[session_id()] = std::make_pair(
      new_host->GetProcess()->GetID(),
      new_host->GetRoutingID());
}

void TabHelper::RenderFrameCreated(content::RenderFrameHost* host) {
  SetTabId(host);
  // Look up the extension API frame ID to force the mapping to be cached.
  // This is needed so that cached information is available for tabId in the
  // filtering callbacks.
  ExtensionApiFrameIdMap::Get()->InitializeRenderFrameData(host);
}

void TabHelper::WebContentsDestroyed() {
  if (browser())
    SetBrowser(nullptr);

  render_view_map_.erase(session_id());
}

void TabHelper::SetTabId(content::RenderFrameHost* render_frame_host) {
  render_frame_host->Send(
      new ExtensionMsg_SetTabId(render_frame_host->GetRoutingID(),
                                session_id()));
}

int32_t TabHelper::session_id() const {
  return SessionTabHelper::FromWebContents(web_contents())->session_id().id();
}

void TabHelper::DidCloneToNewWebContents(
    content::WebContents* old_web_contents,
    content::WebContents* new_web_contents) {
  // When the WebContents that this is attached to is cloned,
  // give the new clone a TabHelper and copy state over.
  CreateForWebContents(new_web_contents);
}

bool TabHelper::ExecuteScriptInTab(mate::Arguments* args) {
  std::string extension_id;
  if (!args->GetNext(&extension_id)) {
    args->ThrowError("extensionId is a required field");
    return false;
  }

  std::string code_string;
  if (!args->GetNext(&code_string)) {
    args->ThrowError("codeString is a required field");
    return false;
  }

  base::DictionaryValue options;
  if (!args->GetNext(&options)) {
    args->ThrowError("options is a required field");
    return false;
  }

  extensions::ScriptExecutor::ResultType result;
  extensions::ScriptExecutor::ExecuteScriptCallback callback;
  if (!args->GetNext(&callback)) {
    callback = extensions::ScriptExecutor::ExecuteScriptCallback();
    result = extensions::ScriptExecutor::NO_RESULT;
  } else {
    result = extensions::ScriptExecutor::JSON_SERIALIZED_RESULT;
  }

  extensions::ScriptExecutor* executor = script_executor();
  if (!executor)
    return false;

  std::string file;
  GURL file_url;
  options.GetString("file", &file);

  std::unique_ptr<base::DictionaryValue> copy = options.CreateDeepCopy();

  if (!file.empty()) {
    ExtensionRegistry* registry =
        ExtensionRegistry::Get(web_contents()->GetBrowserContext());
    if (!registry)
      return false;

    const Extension* extension =
        registry->enabled_extensions().GetByID(extension_id);
    if (!extension)
      return false;

    ExtensionResource resource = extension->GetResource(file);

    if (resource.extension_root().empty() || resource.relative_path().empty()) {
      return false;
    }

    file_url = net::FilePathToFileURL(resource.GetFilePath());

    int resource_id;
    const ComponentExtensionResourceManager*
        component_extension_resource_manager =
            ExtensionsBrowserClient::Get()
                ->GetComponentExtensionResourceManager();

    if (component_extension_resource_manager &&
        component_extension_resource_manager->IsComponentExtensionResource(
            resource.extension_root(),
            resource.relative_path(),
            &resource_id)) {
      const ui::ResourceBundle& rb = ui::ResourceBundle::GetSharedInstance();
      file = rb.GetRawDataResource(resource_id).as_string();
    } else {
      scoped_refptr<FileReader> file_reader(new FileReader(
          resource,
          FileReader::OptionalFileSequenceTask(),  // null callback.
          base::Bind(&TabHelper::ExecuteScript, base::Unretained(this),
            extension_id, base::Passed(&copy), result, callback, file_url)));
      file_reader->Start();
      return true;
    }
  }

  ExecuteScript(extension_id, std::move(copy), result, callback, file_url,
      true, std::make_unique<std::string>(file.empty() ? code_string : file));
  return true;
}

void TabHelper::ExecuteScript(
    const std::string extension_id,
    std::unique_ptr<base::DictionaryValue> options,
    extensions::ScriptExecutor::ResultType result,
    extensions::ScriptExecutor::ExecuteScriptCallback callback,
    const GURL& file_url,
    bool success,
    std::unique_ptr<std::string> code_string) {
  extensions::ScriptExecutor* executor = script_executor();

  bool all_frames = false;
  options->GetBoolean("allFrames", &all_frames);
  extensions::ScriptExecutor::FrameScope frame_scope =
      all_frames
          ? extensions::ScriptExecutor::INCLUDE_SUB_FRAMES
          : extensions::ScriptExecutor::SINGLE_FRAME;

  int frame_id = extensions::ExtensionApiFrameIdMap::kTopFrameId;
  options->GetInteger("frameId", &frame_id);

  bool match_about_blank = false;
  options->GetBoolean("matchAboutBlank", &match_about_blank);

  bool main_world = false;
  options->GetBoolean("mainWorld", &main_world);

  extensions::UserScript::RunLocation run_at =
    extensions::UserScript::UNDEFINED;
  std::string run_at_string = "undefined";
  options->GetString("runAt", &run_at_string);
  if (run_at_string == "document_start") {
    run_at = extensions::UserScript::DOCUMENT_START;
  } else if (run_at_string == "document_end") {
    run_at = extensions::UserScript::DOCUMENT_END;
  } else if (run_at_string == "document_idle") {
    run_at = extensions::UserScript::DOCUMENT_IDLE;
  }

  executor->ExecuteScript(
      HostID(HostID::EXTENSIONS, extension_id),
      extensions::ScriptExecutor::JAVASCRIPT,
      *code_string,
      frame_scope,
      frame_id,
      match_about_blank ? extensions::ScriptExecutor::MATCH_ABOUT_BLANK
                        : extensions::ScriptExecutor::DONT_MATCH_ABOUT_BLANK,
      run_at,
      main_world ? extensions::ScriptExecutor::MAIN_WORLD :
                   extensions::ScriptExecutor::ISOLATED_WORLD,
      extensions::ScriptExecutor::DEFAULT_PROCESS,
      GURL(),  // No webview src.
      file_url,  // No file url.
      false,  // user gesture
      base::nullopt,  // css_origin
      result,
      callback);
}

int TabHelper::get_index() const {
  if (browser())
    return browser()->tab_strip_model()->GetIndexOfWebContents(web_contents());

  return index_;
}

// static
content::WebContents* TabHelper::GetTabById(int32_t tab_id) {
  auto search = render_view_map_.find(tab_id);
  if (search == render_view_map_.end())
    return NULL;

  content::RenderViewHost* rvh =
      content::RenderViewHost::FromID(search->second.first,
                                      search->second.second);

  if (rvh) {
    return content::WebContents::FromRenderViewHost(rvh);
  } else {
    return NULL;
  }
}

// static
content::WebContents* TabHelper::GetTabById(int32_t tab_id,
                          content::BrowserContext* browser_context) {
  auto contents = GetTabById(tab_id);
  if (contents) {
    if (extensions::ExtensionsBrowserClient::Get()->IsSameContext(
                                      browser_context,
                                      contents->GetBrowserContext())) {
      if (tab_id == extensions::TabHelper::IdForTab(contents))
        return contents;
    }
  }
  return NULL;
}

// static
int32_t TabHelper::IdForTab(const content::WebContents* tab) {
  return SessionTabHelper::IdForTab(tab).id();
}

// static
int32_t TabHelper::IdForWindowContainingTab(
    const content::WebContents* tab) {
  return SessionTabHelper::IdForWindowContainingTab(tab).id();
}

}  // namespace extensions
