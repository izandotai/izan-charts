// The acceptance shell: a synthetic random walk streamed through the
// full pane stack — candles, EMA/BOLL overlays, volume, MACD, a
// reference line and a couple of markers. Double-click friendly
// (windows subsystem).

#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <imgui.h>
#include <implot.h>

#include <cmath>
#include <cstdlib>

#include "charts/charts.hpp"

int main()
{
    if (!glfwInit())
        return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    GLFWwindow* window
        = glfwCreateWindow(1280, 800, "izan-charts smoke", nullptr, nullptr);
    if (!window)
        return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    izan::charts::Series series(1.0, 2000); // one-second bars
    izan::charts::IndicatorSet ind;
    izan::charts::Chart chart;

    double t = 0.0, px = 100.0;
    unsigned rng = 1234567u;
    auto frand = [&rng] {
        rng = rng * 1664525u + 1013904223u;
        return double(rng >> 8) / double(1u << 24) - 0.5;
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Four ticks a frame keeps the tape lively at 60fps.
        for (int i = 0; i < 4; ++i) {
            t += 0.25;
            px *= 1.0 + frand() * 0.0008;
            series.push_tick(t, px, std::fabs(frand()) * 5.0);
        }
        chart.ref_price = 100.0;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##chart-host", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        chart.draw("##smoke", series, ind);
        ImGui::End();

        ImGui::Render();
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
