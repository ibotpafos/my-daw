#pragma once
#include "export/dawproject_model.hpp"
#include <string>
#include <string_view>

namespace daw::dawproject {
// XML payloads only. The container writer is responsible for placing these at
// project.xml, metadata.xml and loss-report.json and for materializing the
// referenced audio/<media-id>.wav assets.
std::string projectXml(const ExportModel&,std::string_view appVersion);
std::string metadataXml(std::string_view title);
std::string lossReportJson(const LossReport&);
}
