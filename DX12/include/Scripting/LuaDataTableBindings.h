#pragma once

// DataTable.Load / Reload / Get / Rows / Version / Refused (Gameplay/DataTable.h).

namespace sol { class state; }

void RegisterLuaDataTableBindings(sol::state& lua);
