#pragma once

#include "mesh_parser.h"
#include <vector>
#include <cstdint>
#include <string>

// Parse BSP geometry from a UModel export's serial data (C.1 spec).
// Returns one or more ParsedMesh chunks (split at 60k vertices for uint16 indices).
// The BSP meshes represent the level's static architecture (walls, floors, ceilings).
std::vector<ParsedMesh> ParseBSPGeometry(const uint8_t* serialData, int serialSize,
                                         const std::vector<std::string>& names);
