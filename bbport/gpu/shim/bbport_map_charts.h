// SPDX-License-Identifier: GPL-2.0-or-later
// bbport co-op: the map screen's area pictures (bbport_map_art.h) as the overlay asks for them:
// read from user/map/ when drawn before, else drawn by a worker thread of low priority from the
// game's files and kept there; and the names of the lamps and areas in the game's own text, in the
// language the port's menus use.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "bbport_map_art.h"

namespace BbMapCharts {

/// The area's picture once it is ready; nothing while it is still being drawn (asking starts it)
/// or when it can't be (Failed: no such map in the game's files). Kaniel 2026-10-19 ("can we
/// prerender it?"): `packed_map | view`, view 1 to 4, asks for the 3D view's picture from that
/// side (BbMapArt::ViewOf).
std::shared_ptr<const BbMapArt::Chart> Get(std::uint32_t packed_map);
bool Failed(std::uint32_t packed_map);
/// Kaniel 2026-10-19 (the live 3D map): the area's thinned models (BbMapArt::MeshOf, kept in
/// user/map/ as <area>.bbmesh), as Get: nothing while they are made (asking starts it).
std::shared_ptr<const BbMapArt::AreaMesh> GetMesh(std::uint32_t packed_map);
/// Kaniel 2026-10-21 (ray tracing): the same models on a finer grid for the rays (0.03 m, kept as
/// <area>.rtmesh3), as GetMesh.
std::shared_ptr<const BbMapArt::AreaMesh> GetRayMesh(std::uint32_t packed_map);

/// A lamp's name (menu.msgbnd FMG 30) and an area's (item.msgbnd FMG 19) by their text ids, ""
/// when the game's text has none.
std::string LampName(std::int32_t name_id);
std::string AreaName(std::int32_t name_id);

} // namespace BbMapCharts
