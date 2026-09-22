#pragma once
#include "session.h"
#include <nlohmann/json.hpp>
namespace av {
using Json = nlohmann::json;
Json tool_schemas();
Command parse_command(const Json&);
std::string pipe_call(const std::wstring& pipe,const std::string& request);
}
