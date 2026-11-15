// Throwaway native toolkit performance spike; not the generative UI implementation.
// Builds a representative offscreen surface and reports local timings. The diagram and chart are placeholders;
// this does not establish their final accessibility, data validation or baseline hardware performance.
#include <windows.h>
#include <psapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ui/canvas.h"
#include "ui/elements.h"
#include "ui/richtext.h"
#include "ui/text16.h"
#include "ui/window.h"

using namespace wb;
using namespace wb::ui;

class Sketch : public Node {
public:
    explicit Sketch(bool chart) : chart_(chart) {
        height = Sizing::fixed;
        fixed_h = 180;
        role = Role::group;
        name = chart ? u"Chart prototype, 100 data points" : u"Diagram prototype, 4 nodes";
    }
    void draw(Canvas& c) override {
        const auto f = frame();
        if (chart_) {
            for (int i = 1; i < 100; ++i) {
                float x1 = f.x + (i - 1) * f.w / 100.f, x2 = f.x + i * f.w / 100.f;
                c.line(x1, f.y + 150.f - ((i - 1) % 17) * 7.f, x2, f.y + 150.f - (i % 17) * 7.f, Paint::accent, 2.f);
            }
        } else {
            for (int i = 0; i < 4; ++i) {
                float x = f.x + 10.f + i * 110.f;
                c.round({x, f.y + 30.f, 80.f, 40.f}, 4.f, Paint::accent_surface, Paint::border);
                if (i) c.line(x - 30.f, f.y + 50.f, x, f.y + 50.f, Paint::text, 2.f);
            }
        }
    }
private:
    bool chart_;
};

struct Surface {
    ScrollView* feed;
    Checkbox* first_check;
};

Surface build_surface(Node& content, Ui& ui) {
    auto* feed = content.emplace<ScrollView>("spike-feed");
    feed->height = Sizing::fill;
    feed->padding = Insets::all(8);
    feed->gap = 4;
    for (int i = 0; i < 200; ++i) feed->emplace<Label>(ui, u"Synthetic Thread message", TextStyle::body);
    auto* table = feed->emplace<MarkdownView>(ui, u"Synthetic table");
    std::string markdown = "| Project | Cost |\n|---|---|\n";
    for (int i = 0; i < 100; ++i) markdown += "| Project " + std::to_string(i) + " | " + std::to_string(i * 10) + " |\n";
    table->set_markdown(markdown);
    feed->emplace<Sketch>(true);
    feed->emplace<Sketch>(false);
    Checkbox* first_check = nullptr;
    for (int i = 0; i < 20; ++i) {
        auto* check = feed->emplace<Checkbox>(ui, "choice-" + std::to_string(i), u"Synthetic option");
        if (!first_check) first_check = check;
    }
    return {feed, first_check};
}

bool uia_probe() {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HWND hwnd = nullptr;
    DWORD window_thread = 0;
    std::thread window([&] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        window_thread = GetCurrentThreadId();
        Window::Options options;
        options.title = L"SkoWork generative UI prototype test";
        options.width = 1100; options.height = 800; options.lang = "en"; options.show = false;
        const SystemColors colors{0xFFFFFF, 0x000000, 0x0078D7, 0xFFFFFF, 0x0066CC, 0x6D6D6D, 0xF0F0F0, 0x000000};
        options.theme = make_theme(false, false, colors);
        auto created = Window::create(options);
        if (created.ok()) {
            auto w = std::move(created).value();
            build_surface(w->content(), w->ui());
            w->tree().layout_if_needed();
            hwnd = w->hwnd();
            SetEvent(ready);
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        } else SetEvent(ready);
        CoUninitialize();
    });
    WaitForSingleObject(ready, INFINITE);
    bool success = false;
    if (hwnd) {
        std::thread client([&] {
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
            {
                constexpr CLSID clsid = {0xe22ad333, 0xb25f, 0x460c, {0x83, 0xd0, 0x05, 0x81, 0x10, 0x73, 0x95, 0xc9}};
                com_ptr<IUIAutomation5> uia;
                com_ptr<IUIAutomationElement> root;
                if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation5), uia.put_void())) &&
                    SUCCEEDED(uia->ElementFromHandle(hwnd, root.put())) && root) {
                    auto find_named = [&](const wchar_t* name, CONTROLTYPEID type) {
                        VARIANT v; VariantInit(&v); v.vt = VT_BSTR; v.bstrVal = SysAllocString(name);
                        com_ptr<IUIAutomationCondition> condition;
                        com_ptr<IUIAutomationElement> found;
                        const bool ok = SUCCEEDED(uia->CreatePropertyCondition(UIA_NamePropertyId, v, condition.put())) &&
                            SUCCEEDED(root->FindFirst(TreeScope_Descendants, condition.get(), found.put())) && found;
                        VariantClear(&v);
                        CONTROLTYPEID actual = 0;
                        return ok && SUCCEEDED(found->get_CurrentControlType(&actual)) && actual == type;
                    };
                    success = find_named(L"Chart prototype, 100 data points", UIA_GroupControlTypeId) &&
                        find_named(L"Diagram prototype, 4 nodes", UIA_GroupControlTypeId) &&
                        find_named(L"Synthetic option", UIA_CheckBoxControlTypeId);
                }
            }
            CoUninitialize();
        });
        client.join();
        PostThreadMessageW(window_thread, WM_QUIT, 0, 0);
    }
    window.join();
    CloseHandle(ready);
    return success;
}

int main() {
    DWORD_PTR available = 0, system = 0, selected = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &available, &system)) return 10;
    int cores = 0;
    for (unsigned i = 0; i < sizeof(DWORD_PTR) * 8 && cores < 4; ++i) {
        const DWORD_PTR bit = DWORD_PTR{1} << i;
        if (available & bit) { selected |= bit; ++cores; }
    }
    if (cores != 4 || !SetProcessAffinityMask(GetCurrentProcess(), selected)) return 11;
    DWORD_PTR effective = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &effective, &system) || effective != selected) return 12;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    {
    auto render_result = Renderer::offscreen(1100, 800, 96);
    if (!render_result.ok()) return 2;
    auto renderer = std::move(render_result).value();
    auto ui_result = Ui::create(renderer->dwrite(), "en");
    if (!ui_result.ok()) return 3;
    auto ui = std::move(ui_result).value();
    const SystemColors colors{0xFFFFFF, 0x000000, 0x0078D7, 0xFFFFFF, 0x0066CC, 0x6D6D6D, 0xF0F0F0, 0x000000};
    ui->theme = make_theme(false, false, colors);
    PROCESS_MEMORY_COUNTERS memory_before{};
    memory_before.cb = sizeof(memory_before);
    GetProcessMemoryInfo(GetCurrentProcess(), &memory_before, sizeof(memory_before));
    std::vector<double> first_render;
    for (int i = 0; i < 31; ++i) {
        const auto start = std::chrono::steady_clock::now();
        Tree sample;
        WindowRoot sample_root(sample);
        build_surface(sample_root.content(), *ui);
        sample.layout(renderer->size_dip());
        sample.invalidate_all();
        auto regions = renderer->begin_frame(sample.take_dirty());
        { Canvas c(*renderer, *ui); paint(sample, c, regions); }
        if (!renderer->end_frame().ok()) return 13;
        if (i) first_render.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(first_render.begin(), first_render.end());
    Tree tree;
    WindowRoot root(tree);
    auto [feed, first_check] = build_surface(root.content(), *ui);
    const auto a11y = a11y_problems(tree);
    auto frame = [&] {
        tree.layout(renderer->size_dip());
        tree.invalidate_all();
        auto regions = renderer->begin_frame(tree.take_dirty());
        { Canvas c(*renderer, *ui); paint(tree, c, regions); }
        return renderer->end_frame().ok();
    };
    if (!frame()) return 4;
    tree.set_focus(first_check);
    tree.key({kKeySpace});
    const bool keyboard_toggle = first_check->checked();
    std::vector<double> elapsed;
    for (int i = 0; i < 30; ++i) {
        const auto start = std::chrono::steady_clock::now();
        tree.scroll_to(*feed, (feed->max_scroll() * (i % 10)) / 10.f);
        if (!frame()) return 5;
        elapsed.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(elapsed.begin(), elapsed.end());
    PROCESS_MEMORY_COUNTERS memory_after{};
    memory_after.cb = sizeof(memory_after);
    GetProcessMemoryInfo(GetCurrentProcess(), &memory_after, sizeof(memory_after));
    const bool uia_ok = uia_probe();
    std::printf("{\"affinity_cores\":%d,\"affinity_mask\":%llu,\"first_render_p95_ms\":%.2f,\"first_render_max_ms\":%.2f,\"frames\":%zu,\"frame_p95_ms\":%.2f,\"frame_max_ms\":%.2f,\"working_set_delta_bytes\":%lld,\"tree_accessibility_issues\":%zu,\"keyboard_toggle\":%s,\"uia_names_and_roles\":%s}\n",
        cores, static_cast<unsigned long long>(effective), first_render[28], first_render.back(), elapsed.size(), elapsed[28], elapsed.back(),
        static_cast<long long>(memory_after.WorkingSetSize) - static_cast<long long>(memory_before.WorkingSetSize), a11y.size(),
        keyboard_toggle ? "true" : "false", uia_ok ? "true" : "false");
    }
    CoUninitialize();
    return 0;
}
