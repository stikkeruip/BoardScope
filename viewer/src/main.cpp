// BoardScope viewer: shows the iPhone's camera over USB, with zoom and focus that stay in step
// with the phone. Whichever side moves a control last wins.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <turbojpeg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "link.h"

namespace {

constexpr float panelWidth = 290;
constexpr float zoomStep = 1.1f;
constexpr float fineFocusStep = 0.002f;
constexpr float coarseFocusStep = 0.02f;

/// A control that follows the phone, except while it's held here and until the phone has
/// applied the last change made here.
struct Synced {
    float value = 0;
    std::uint32_t pending = 0;
    bool held = false;

    void follow(float phone, std::uint32_t applied) {
        if (!held && applied >= pending) value = phone;
    }
};

/// Zoom, focus and JPEG quality, kept in step with the phone.
class Controls {
public:
    explicit Controls(PhoneLink& link) : link_(link) {}

    Synced zoom, focus, quality;

    void update(const PhoneLink::Snapshot& snapshot) {
        if (snapshot.connection != connection_) {
            connection_ = snapshot.connection;
            zoom.pending = focus.pending = quality.pending = 0;
        }
        if (!snapshot.state) return;
        state_ = *snapshot.state;
        zoom.follow(state_.zoom, state_.lastCommand);
        focus.follow(state_.focus, state_.lastCommand);
        quality.follow(state_.quality, state_.lastCommand);
    }

    const wire::State& state() const { return state_; }

    void setZoom(float value) {
        zoom.value = std::clamp(value, state_.zoomMin, state_.zoomMax);
        wire::Command command;
        command.zoom = zoom.value;
        zoom.pending = link_.send(command);
    }

    void setFocus(float value) {
        focus.value = std::clamp(value, 0.0f, 1.0f);
        wire::Command command;
        command.focus = focus.value;
        focus.pending = link_.send(command);
    }

    void setQuality(float value) {
        quality.value = std::clamp(value, 0.3f, 0.95f);
        wire::Command command;
        command.quality = quality.value;
        quality.pending = link_.send(command);
    }

private:
    PhoneLink& link_;
    std::uint64_t connection_ = 0;
    wire::State state_;
};

/// Decodes each JPEG straight into a streaming texture and draws it letterboxed.
class FrameView {
public:
    explicit FrameView(SDL_Renderer* renderer) : renderer_(renderer), decoder_(tj3Init(TJINIT_DECOMPRESS)) {}

    ~FrameView() {
        if (texture_) SDL_DestroyTexture(texture_);
        tj3Destroy(decoder_);
    }

    bool show(const std::vector<std::uint8_t>& jpeg) {
        if (tj3DecompressHeader(decoder_, jpeg.data(), jpeg.size()) != 0) return false;
        int width = tj3Get(decoder_, TJPARAM_JPEGWIDTH);
        int height = tj3Get(decoder_, TJPARAM_JPEGHEIGHT);
        if (!texture_ || width != width_ || height != height_) {
            if (texture_) SDL_DestroyTexture(texture_);
            texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
            if (!texture_) return false;
            SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
            SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
            width_ = width;
            height_ = height;
        }
        void* pixels;
        int pitch;
        if (!SDL_LockTexture(texture_, nullptr, &pixels, &pitch)) return false;
        int result = tj3Decompress8(decoder_, jpeg.data(), jpeg.size(), static_cast<unsigned char*>(pixels), pitch, TJPF_RGBA);
        SDL_UnlockTexture(texture_);
        // Warnings, such as a frame cut short, still leave a usable picture.
        return result == 0 || tj3GetErrorCode(decoder_) == TJERR_WARNING;
    }

    void draw(SDL_FRect area, bool stale) {
        if (!texture_ || area.w <= 0 || area.h <= 0) return;
        float scale = std::min(area.w / width_, area.h / height_);
        SDL_FRect target{area.x + (area.w - width_ * scale) / 2, area.y + (area.h - height_ * scale) / 2,
                         width_ * scale, height_ * scale};
        Uint8 shade = stale ? 90 : 255;
        SDL_SetTextureColorMod(texture_, shade, shade, shade);
        SDL_RenderTexture(renderer_, texture_, nullptr, &target);
    }

    bool empty() const { return texture_ == nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    SDL_Renderer* renderer_;
    tjhandle decoder_;
    SDL_Texture* texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

const char* heatLabel(const wire::State& state) {
    if (state.pausedForHeat) return "Too hot, camera paused";
    switch (state.thermal) {
    case wire::Thermal::nominal: return "Cool";
    case wire::Thermal::fair: return "Warm";
    case wire::Thermal::serious: return "Hot, 15 fps";
    case wire::Thermal::critical: return "Too hot";
    }
    return "Unknown";
}

ImVec4 heatColor(wire::Thermal thermal) {
    switch (thermal) {
    case wire::Thermal::nominal: return {0.35f, 0.85f, 0.4f, 1};
    case wire::Thermal::fair: return {0.95f, 0.85f, 0.3f, 1};
    case wire::Thermal::serious: return {1.0f, 0.6f, 0.2f, 1};
    case wire::Thermal::critical: return {1.0f, 0.3f, 0.3f, 1};
    }
    return {0.6f, 0.6f, 0.6f, 1};
}

/// Keyboard shortcuts. Returns false for keys it doesn't use.
bool handleKey(const SDL_KeyboardEvent& key, Controls& controls, SDL_Window* window, bool& showPanel) {
    bool shift = key.mod & SDL_KMOD_SHIFT;
    switch (key.key) {
    case SDLK_EQUALS:
    case SDLK_PLUS:
    case SDLK_KP_PLUS:
        controls.setZoom(controls.zoom.value * zoomStep);
        return true;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        controls.setZoom(controls.zoom.value / zoomStep);
        return true;
    case SDLK_UP:
        controls.setFocus(controls.focus.value + (shift ? coarseFocusStep : fineFocusStep));
        return true;
    case SDLK_DOWN:
        controls.setFocus(controls.focus.value - (shift ? coarseFocusStep : fineFocusStep));
        return true;
    case SDLK_TAB:
        if (!key.repeat) showPanel = !showPanel;
        return true;
    case SDLK_F11:
        if (!key.repeat) SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
        return true;
    case SDLK_ESCAPE:
        SDL_SetWindowFullscreen(window, false);
        return true;
    default:
        if (key.key >= SDLK_1 && key.key <= SDLK_8 && !key.repeat) {
            controls.setZoom(static_cast<float>(key.key - SDLK_1 + 1));
            return true;
        }
        return false;
    }
}

void drawPanel(Controls& controls, bool connected, float height) {
    const ImGuiIO& io = ImGui::GetIO();
    float width = panelWidth * ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowPos({io.DisplaySize.x - width, 0});
    ImGui::SetNextWindowSize({width, height});
    ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushItemWidth(-1);
    const wire::State& state = controls.state();
    ImGui::BeginDisabled(!connected);

    ImGui::SeparatorText("Zoom");
    float zoom = controls.zoom.value;
    if (ImGui::SliderFloat("##zoom", &zoom, state.zoomMin, state.zoomMax, "%.1fx", ImGuiSliderFlags_Logarithmic)) {
        controls.setZoom(zoom);
    }
    controls.zoom.held = ImGui::IsItemActive();
    for (float preset : {1.0f, 2.0f, 4.0f, 8.0f}) {
        if (preset > state.zoomMax + 0.01f) break;
        if (preset != 1.0f) ImGui::SameLine();
        char label[8];
        std::snprintf(label, sizeof label, "%gx", preset);
        if (ImGui::Button(label, {ImGui::GetFrameHeight() * 1.8f, 0})) controls.setZoom(preset);
    }

    ImGui::SeparatorText("Focus");
    float focus = controls.focus.value;
    if (ImGui::SliderFloat("##focus", &focus, 0, 1, "%.3f")) controls.setFocus(focus);
    controls.focus.held = ImGui::IsItemActive();
    ImGui::TextDisabled("Near");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Far").x);
    ImGui::TextDisabled("Far");
    float stepWidth = (ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4;
    if (ImGui::Button("<<", {stepWidth, 0})) controls.setFocus(controls.focus.value - coarseFocusStep);
    ImGui::SameLine();
    if (ImGui::Button("<", {stepWidth, 0})) controls.setFocus(controls.focus.value - fineFocusStep);
    ImGui::SameLine();
    if (ImGui::Button(">", {stepWidth, 0})) controls.setFocus(controls.focus.value + fineFocusStep);
    ImGui::SameLine();
    if (ImGui::Button(">>", {stepWidth, 0})) controls.setFocus(controls.focus.value + coarseFocusStep);

    ImGui::SeparatorText("JPEG quality");
    int quality = static_cast<int>(std::lround(controls.quality.value * 100));
    if (ImGui::SliderInt("##quality", &quality, 30, 95, "%d%%")) controls.setQuality(quality / 100.0f);
    controls.quality.held = ImGui::IsItemActive();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Phone");
    if (connected) {
        ImGui::Text("%s camera, light %s", state.telephoto ? "Telephoto" : "Main", state.torchOn ? "on" : "off");
        ImGui::Text("Heat:");
        ImGui::SameLine();
        ImGui::TextColored(heatColor(state.thermal), "%s", heatLabel(state));
    } else {
        ImGui::TextDisabled("Not connected");
    }

    ImGui::SeparatorText("Keys");
    float actionColumn = ImGui::CalcTextSize("Shift+Up/Down").x + ImGui::GetFontSize();
    auto shortcut = [actionColumn](const char* keys, const char* action) {
        ImGui::TextUnformatted(keys);
        ImGui::SameLine(actionColumn);
        ImGui::TextDisabled("%s", action);
    };
    shortcut("+ / -, wheel", "zoom");
    shortcut("1 to 8", "zoom 1x to 8x");
    shortcut("Up / Down", "focus, fine");
    shortcut("Shift+Up/Down", "focus, coarse");
    shortcut("Tab", "hide controls");
    shortcut("F11", "full screen");
    ImGui::PopItemWidth();
    ImGui::End();
}

void drawStatusLine(const PhoneLink::Snapshot& link, double shownFps, const FrameView& view, float top) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, top});
    ImGui::SetNextWindowSize({io.DisplaySize.x, io.DisplaySize.y - top});
    ImGui::Begin("Status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    // The tunnel opens before the app answers; it only counts once the phone has said how it is.
    bool connected = link.status == PhoneLink::Status::connected && link.state;
    ImVec4 dot = connected ? ImVec4(0.35f, 0.85f, 0.4f, 1) : ImVec4(1.0f, 0.6f, 0.2f, 1);
    ImGui::TextColored(dot, connected ? "Connected over USB" : "Not connected");
    ImGui::SameLine();
    if (link.status == PhoneLink::Status::connected && !link.state) {
        ImGui::TextUnformatted("Waiting for BoardScope to answer…");
    } else if (!connected) {
        ImGui::TextUnformatted(link.problem.c_str());
    } else {
        std::string line = "   " + std::to_string(static_cast<int>(std::lround(shownFps))) + " fps";
        if (!view.empty()) line += "   " + std::to_string(view.width()) + "x" + std::to_string(view.height());
        line += "   " + std::to_string(link.frameBytes / 1024) + " KB/frame";
        char roundTrip[48] = "   round trip --";
        if (link.roundTripMs >= 0) std::snprintf(roundTrip, sizeof roundTrip, "   round trip %.1f ms", link.roundTripMs);
        line += roundTrip;
        ImGui::TextUnformatted(line.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted("   heat");
        ImGui::SameLine();
        ImGui::TextColored(heatColor(link.state->thermal), "%s", heatLabel(*link.state));
    }
    ImGui::End();
}

}  // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    SDL_Window* window = SDL_CreateWindow("BoardScope", static_cast<int>(1440 * scale), static_cast<int>(860 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_Log("Couldn't open a window: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowMinimumSize(window, 800, 480);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefaultVector();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontSizeBase = 15;
    style.WindowBorderSize = 0;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    PhoneLink link;
    Controls controls(link);
    FrameView view(renderer);
    std::vector<std::uint8_t> jpeg;
    bool showPanel = true;
    double shownFps = 0;
    int shownFrames = 0;
    auto fpsStart = std::chrono::steady_clock::now();

    for (bool running = true; running;) {
        PhoneLink::Snapshot snapshot = link.snapshot();
        bool connected = snapshot.status == PhoneLink::Status::connected && snapshot.state.has_value();
        controls.update(snapshot);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!io.WantTextInput && (connected || event.key.key == SDLK_TAB || event.key.key == SDLK_F11 || event.key.key == SDLK_ESCAPE)) {
                    handleKey(event.key, controls, window, showPanel);
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (!io.WantCaptureMouse && connected && event.wheel.y != 0) {
                    controls.setZoom(controls.zoom.value * std::pow(zoomStep, event.wheel.y));
                }
                break;
            default:
                break;
            }
        }

        if (link.takeFrame(jpeg) && view.show(jpeg)) ++shownFrames;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - fpsStart).count();
        if (elapsed >= 1) {
            shownFps = shownFrames / elapsed;
            shownFrames = 0;
            fpsStart = now;
        }
        if (snapshot.status != PhoneLink::Status::connected) shownFps = 0;

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        float statusTop = io.DisplaySize.y - ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().WindowPadding.y;
        float imageWidth = io.DisplaySize.x - (showPanel ? panelWidth * style.FontScaleDpi : 0);
        if (showPanel) drawPanel(controls, connected, statusTop);
        drawStatusLine(snapshot, shownFps, view, statusTop);

        bool stale = snapshot.status != PhoneLink::Status::connected;
        const char* message = nullptr;
        if (stale) {
            message = snapshot.problem.empty() ? "Looking for the iPhone…" : snapshot.problem.c_str();
        } else if (view.empty()) {
            message = "Waiting for the first picture…";
        } else if (snapshot.state && snapshot.state->pausedForHeat) {
            message = "The camera is paused: the phone is too hot. It resumes when it cools down.";
        }
        if (message) {
            ImFont* font = ImGui::GetFont();
            float size = ImGui::GetFontSize() * 1.4f;
            ImVec2 extent = font->CalcTextSizeA(size, imageWidth * 0.9f, imageWidth * 0.9f, message);
            ImVec2 at{(imageWidth - extent.x) / 2, (statusTop - extent.y) / 2};
            ImGui::GetBackgroundDrawList()->AddText(font, size, at, IM_COL32(230, 230, 230, 255), message, nullptr, imageWidth * 0.9f);
        }
        ImGui::Render();

        // Everything is drawn in window points; the scale maps them to the display's pixels.
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 12, 12, 12, 255);
        SDL_RenderClear(renderer);
        view.draw({0, 0, imageWidth, statusTop}, stale);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
