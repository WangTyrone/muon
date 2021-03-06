#ifndef BRIGHTRAY_INSPECTABLE_WEB_CONTENTS_DELEGATE_H_
#define BRIGHTRAY_INSPECTABLE_WEB_CONTENTS_DELEGATE_H_

#include <string>
#include "base/files/file_path.h"

namespace brightray {

class InspectableWebContentsDelegate {
 public:
  virtual ~InspectableWebContentsDelegate() {}

  // Requested by WebContents of devtools.
  virtual void DevToolsReloadPage() {}
  virtual void DevToolsSaveToFile(
      const std::string& url, const std::string& content, bool save_as) {}
  virtual void DevToolsAppendToFile(
      const std::string& url, const std::string& content) {}
  virtual void DevToolsRequestFileSystems() {}
  virtual void DevToolsAddFileSystem(
      const base::FilePath& file_system_path, const std::string& type) {}
  virtual void DevToolsRemoveFileSystem(
      const base::FilePath& file_system_path) {}
  virtual void DevToolsIndexPath(
      int request_id,
      const std::string& file_system_path,
      const std::vector<std::string>& excluded_folders) {}
  virtual void DevToolsStopIndexing(int request_id) {}
  virtual void DevToolsSearchInPath(
      int request_id,
      const std::string& file_system_path,
      const std::string& query) {}
};

}  // namespace brightray

#endif  // BRIGHTRAY_INSPECTABLE_WEB_CONTENTS_DELEGATE_H_
