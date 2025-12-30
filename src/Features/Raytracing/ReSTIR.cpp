#include "ReSTIR.h"

void ReSTIR::DrawReSTIRSettings()
{
    if (ImGui::CollapsingHeader("ReSTIR Settings"))
    {
        ImGui::Checkbox("Enable ReSTIR-DI", &restirSettings.EnableReSTIRDI);
        ImGui::Checkbox("Spatial Reuse", &restirSettings.SpatialReuse);
        ImGui::Checkbox("Temporal Reuse", &restirSettings.TemporalReuse);
        ImGui::SliderInt("Initial Candidate Count", &restirSettings.InitialCandidateCount, 1, 32, "%d", ImGuiSliderFlags_AlwaysClamp);
    }
}

void ReSTIR::ExecuteReSTIRPass()
{
    // Placeholder for executing the ReSTIR pass logic
}