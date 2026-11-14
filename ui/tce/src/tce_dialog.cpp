// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/ui_tce/tce_dialog.h"

#include "sonora/api/elastic.h"
#include "sonora/ui_tce/tce_ratio.h"

namespace sonora::ui::tce {

bool TceDialog::apply(double ratio, const std::string& algorithm, std::string& why) {
    if (!(ratio >= api::kMinElasticRatio) || ratio > api::kMaxElasticRatio) {
        why = "a ratio of " + api::elastic_detail::format_micro(ratio) + " is outside " +
              api::elastic_detail::format_micro(api::kMinElasticRatio) + ".." +
              api::elastic_detail::format_micro(api::kMaxElasticRatio);
        return false;
    }
    const int64_t length = tce::resulting_length(source_, ratio);
    if (length > api::kMaxElasticFrames) {
        why = "the stretched region would exceed the elastic length limit";
        return false;
    }
    const std::string words =
        "elastic ratio " + api::elastic_detail::format_micro(ratio) + " " + algorithm;
    if (!elastic_command(ec_, deps_, words, why)) return false;
    result_ = length;
    return true;
}

}  // namespace sonora::ui::tce
