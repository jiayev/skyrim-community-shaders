#pragma once

class JiayeStatement
{
public:
    static JiayeStatement* GetSingleton()
    {
        static JiayeStatement instance;
        return &instance;
    }

    void DrawJSInfo() {
        if (ImGui::TreeNodeEx("给中文用户的一些说明")) {
            ImGui::Text("本社区着色器AIO版本是多位开发者共同努力的成果，由Jiaye发布于社区着色器DC服务器。");
            ImGui::Text("在任何其他渠道（除本人亲自发布以外）获取的本社区着色器AIO版本均与本人无关。");
            ImGui::Text("社区着色器是基于GPLv3协议发布的开源项目，永久免费。");
            ImGui::Text("如果你是在付费群组、付费整合包或其他付费渠道获取的本社区着色器AIO版本，你已经被骗了。");
            ImGui::Text("本人（Jiaye）永久保持以下立场：");
            ImGui::Text("1. 本社区着色器AIO版本是免费开源的，如果你是通过付费渠道获取的，请立即要求退款。");
            ImGui::Text("2. 反对任何形式的付费整合包和付费、赞助门槛群组，或是任何涉及出售整合包行为的人。");
            ImGui::Text("3. 请尊重开源社区精神，尊重开发者的劳动成果。");
            ImGui::Text("4. 如果你有任何问题或建议，请在社区着色器DC服务器中提出。本人不对你在其它任何渠道获取本产品导致的问题负责。");
            ImGui::Text("社区着色器DC永久邀请链接：");
            ImGui::Text("https://discord.com/invite/nkrQybAsyy");
            ImGui::Text("Jiaye的社区着色器讨论群：1059023812");
            ImGui::TreePop();
        }
    }

    void WelcomePage() {
        
    }
};