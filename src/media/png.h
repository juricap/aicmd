#pragma once
#include "backend.h"
namespace av {
// Agent image responses are bounded independently of full-resolution clipboard images.
std::string preview_png(const Pixels&,int& width,int& height);
}
