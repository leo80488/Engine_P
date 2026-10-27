// NavEditorModule: the Nav module's editor half - Tools > Bake > NavMesh... and its popup (Recast /
// Detour over the world's Mesh colliders). The NavAgent inspector is the core's.

#include "Editor/EditorModule.h"

#include "Editor/ImGuiWidgets.h"     // EditorUI::DragFloat / DragInt
#include "Nav/NavMeshSystem.h"
#include "System/Log.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <string>

class NavEditorModule final : public Editor::IEditorModule
{
public:
    void AddMenuItems(std::vector<Editor::ModuleMenuItem>& out) override
    {
        out.push_back({ "Tools/Bake", "NavMesh...", "Bake NavMesh...", [this] { m_openRequest = true; } });
    }

    // Inputs are the world's Mesh ColliderComponents: apply collision with "Bake Collision Meshes"
    // first, then bake nav here.
    void DrawPanels(const Editor::ModuleHost& host) override
    {
        if (m_openRequest)
        {
            ImGui::OpenPopup("Bake NavMesh");
            m_openRequest = false;
        }
        Nav::NavMeshSystem* nav = Nav::ActiveNavMesh();
        {
            static Nav::BuildParams s_navParams;
            static Nav::BuildStats  s_navStats;
            static char             s_navOut[260]    = "asset/nav/world.inav";
            static char             s_navLoadPath[260] = "asset/nav/world.inav";

            ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("Bake NavMesh", nullptr,
                                        ImGuiWindowFlags_AlwaysAutoResize))
            {
                const bool hasWorld = (host.world != nullptr);
                const bool hasNav   = (nav != nullptr);

                ImGui::TextWrapped(
                    "Feeds every Mesh ColliderComponent in the world through Recast "
                    "(voxel rasterise → contour → polymesh → detail mesh → Detour "
                    "tile) and writes a .inav. Runtime FindPath / NavAgent path-"
                    "following go live as soon as Build completes.");
                ImGui::Separator();

                // Quick status: what is currently loaded.
                if (hasNav && nav->IsReady())
                {
                    const std::string& src = nav->SourcePath();
                    ImGui::TextColored(ImVec4(0.6f, 1.f, 0.6f, 1),
                        "NavMesh ready%s%s",
                        src.empty() ? "" : "  source: ",
                        src.empty() ? "" : src.c_str());
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1),
                                        "No navmesh loaded.");
                }
                ImGui::Separator();

                ImGui::TextDisabled("Recast / agent parameters");
                EditorUI::DragFloat("Cell size",       &s_navParams.cellSize,        0.05f, 1.0f, "%.2f");
                EditorUI::DragFloat("Cell height",     &s_navParams.cellHeight,      0.05f, 1.0f, "%.2f");
                EditorUI::DragFloat("Agent height",    &s_navParams.agentHeight,     0.5f, 4.0f, "%.2f");
                EditorUI::DragFloat("Agent radius",    &s_navParams.agentRadius,     0.1f, 2.0f, "%.2f");
                EditorUI::DragFloat("Agent max climb", &s_navParams.agentMaxClimb,   0.05f, 2.0f, "%.2f");
                EditorUI::DragFloat("Max slope (deg)", &s_navParams.agentMaxSlopeDeg,5.0f, 75.0f, "%.0f");
                int minRegion = s_navParams.minRegionArea;
                if (ImGui::InputInt("Min region area (cells^2)", &minRegion))
                    s_navParams.minRegionArea = std::max(0, minRegion);

                // Tile size in cells. 0 = single-tile (fastest for small levels); 32 / 64 / 128 = tiled - Recast
                // splits the world into a grid of (tileSize x cellSize) world units. Use tiles for worlds over
                // 1 km^2.
                int tileSize = s_navParams.tileSize;
                if (EditorUI::DragInt("Tile size (cells, 0 = single tile)",
                                      &tileSize, 0, 256))
                    s_navParams.tileSize = std::max(0, tileSize);
                if (s_navParams.tileSize > 1)
                    ImGui::TextDisabled("  tile world size ≈ %.1f m",
                        s_navParams.tileSize * s_navParams.cellSize);
                ImGui::Separator();

                ImGui::InputText("Output .inav path", s_navOut, sizeof(s_navOut));

                // ---- Action row ----
                const bool canBuild = hasWorld && hasNav;
                ImGui::BeginDisabled(!canBuild);
                if (ImGui::Button("Build NavMesh"))
                {
                    Nav::TriangleSoup soup;
                    if (Nav::BuildSoupFromWorldColliders(*host.world, soup, host.physics))
                    {
                        s_navStats = nav->Build(soup, s_navParams);
                        if (s_navStats.success && s_navOut[0])
                            nav->Save(s_navOut);
                    }
                    else
                    {
                        s_navStats = {};
                        s_navStats.success = false;
                        s_navStats.error   = "no Mesh ColliderComponents in world — "
                                              "apply collision first via "
                                              "Bake Collision Meshes → Use baked as collision";
                        LOG_WARNING("NavMesh: nothing to feed Recast — apply collision first");
                    }
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(!hasNav || !nav->IsReady() || s_navOut[0] == '\0');
                if (ImGui::Button("Save"))
                    nav->Save(s_navOut);
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::SetNextItemWidth(220);
                ImGui::InputText("##loadpath", s_navLoadPath, sizeof(s_navLoadPath));
                ImGui::SameLine();
                ImGui::BeginDisabled(!hasNav || s_navLoadPath[0] == '\0');
                if (ImGui::Button("Load"))
                {
                    if (nav->Load(s_navLoadPath))
                    {
                        s_navStats = {};   // stats from a previous Build do not describe the new mesh
                        s_navStats.success = true;
                    }
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(!hasNav || !nav->IsReady());
                if (ImGui::Button("Clear"))
                {
                    nav->Clear();
                    s_navStats = {};
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                if (ImGui::Button("Close"))
                    ImGui::CloseCurrentPopup();

                // ---- Result line ----
                if (s_navStats.inputTris > 0 || !s_navStats.error.empty() || s_navStats.success)
                {
                    ImGui::Separator();
                    if (s_navStats.success && s_navStats.inputTris > 0)
                        ImGui::TextColored(ImVec4(0.6f, 1.f, 0.6f, 1),
                            "OK   in: %u verts %u tris    out: %u polys, %u/%u tiles    (%.1f ms)",
                            s_navStats.inputVerts, s_navStats.inputTris,
                            s_navStats.polyCount,
                            s_navStats.tilesBuilt, s_navStats.tilesTotal,
                            s_navStats.buildMs);
                    else if (s_navStats.success)
                        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.f, 1),
                            "Loaded.");
                    else
                        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1),
                            "FAIL: %s", s_navStats.error.c_str());
                }

                ImGui::EndPopup();
            }
        }
    }

private:
    bool m_openRequest = false;   // Tools > Bake > NavMesh...
};

EDITOR_MODULE(NavEditorModule)
