#include "Graphics/ShadingModel.h"
#include "Graphics/ShadingModelTable.h"

// The table's `display` column; "Unknown" outside the enum (ShadingModels::kUnknown).
const char* ShadingModelName(ShadingModel m)
{
    return ShadingModels::Of(m).display;
}
