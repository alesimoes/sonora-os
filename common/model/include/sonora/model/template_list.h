// SPDX-License-Identifier: GPL-3.0-or-later
//
// The template browser's list (ADR-0146): which folders of the templates
// folder are templates, which are categories, and what each template's file
// declares. The model does no I/O, so the folder is read through
// TemplateFolder, which the interface implements on the real disk and a
// test on a map.
#pragma once

#include <string>
#include <vector>

#include "sonora/model/session_template.h"

namespace sonora::model {

class TemplateFolder {
public:
    virtual ~TemplateFolder() = default;
    // The folders directly in `folder` that may hold a template: real
    // folders, never a link, whose names pass the session-name check.
    virtual std::vector<std::string> subfolders(const std::string& folder) = 0;
    // The whole of `file` when it is a regular file and not a link; false
    // when it is not there, is a link, or cannot be read.
    virtual bool read_file(const std::string& file, std::string& bytes) = 0;
};

// Every template under `root`, one category level deep, sorted by category
// (uncategorised first) and then by name. A folder holding <Name>.sonora
// is a template and never a category; a folder whose file is not a session
// file is left out (ADR-0146 §3). Paths are joined with '/'.
std::vector<TemplateInfo> list_templates(TemplateFolder& folder, const std::string& root);

}  // namespace sonora::model
