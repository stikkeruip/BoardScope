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
    explicit Controls(PhoneLink& link) : link_(link) {
        // Until the phone says otherwise, show the defaults rather than zeros.
        zoom.value = state_.zoom;
        focus.value = state_.focus;
        quality.value = state_.quality;
        lightLevel.value = state_.torchLevel;
    }

    Synced zoom, focus, quality;
    /// The light, 1 for on, and its brightness.
    Synced light, lightLevel;

    void update(const PhoneLink::Snapshot& snapshot) {
        if (snapshot.connection != connection_) {
            connection_ = snapshot.connection;
            zoom.pending = focus.pending = quality.pending = light.pending = lightLevel.pending = 0;
        }
        if (!snapshot.state) return;
        state_ = *snapshot.state;
        zoom.follow(state_.zoom, state_.lastCommand);
        focus.follow(state_.focus, state_.lastCommand);
        quality.follow(state_.quality, state_.lastCommand);
        light.follow(state_.torchOn ? 1.0f : 0.0f, state_.lastCommand);
        lightLevel.follow(state_.torchLevel, state_.lastCommand);
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

    void setLight(bool on) {
        light.value = on ? 1.0f : 0.0f;
        wire::Command command;
        command.torchOn = on;
        light.pending = link_.send(command);
    }

    void setLightLevel(float value) {
        lightLevel.value = std::clamp(value, 0.05f, 1.0f);
        wire::Command command;
        command.torchLevel = lightLevel.value;
        lightLevel.pending = link_.send(command);
    }

private:
    PhoneLink& link_;
    std::uint64_t connection_ = 0;
    wire::State state_;
};

/// What the viewer remembers between runs, as key=value lines in one small file.
struct Settings {
    /// Quarter turns clockwise, from 0 to 3. Remembered since the phone stays put on its arm.
    int quarterTurns = 0;
    PhoneLink::Options link;

    void load() {
        std::size_t size = 0;
        void* data = SDL_LoadFile(path().c_str(), &size);
        if (!data) return;
        std::string text(static_cast<const char*>(data), size);
        SDL_free(data);
        for (std::size_t start = 0; start < text.size();) {
            std::size_t end = std::min(text.find('\n', start), text.size());
            std::string line = text.substr(start, end - start);
            start = end + 1;
            std::size_t equals = line.find('=');
            if (equals == std::string::npos) continue;
            std::string key = line.substr(0, equals);
            std::string value = line.substr(equals + 1);
            if (key == "rotation") {
                quarterTurns = std::clamp(std::atoi(value.c_str()), 0, 3);
            } else if (key == "connection") {
                link.mode = value == "usb" ? PhoneLink::Mode::usb : value == "wifi" ? PhoneLink::Mode::wifi : PhoneLink::Mode::automatic;
            } else if (key == "pairing-code") {
                link.pairingCode = value;
            } else if (key == "phone-address") {
                link.address = value;
            }
        }
    }

    void save() const {
        const char* mode = link.mode == PhoneLink::Mode::usb ? "usb" : link.mode == PhoneLink::Mode::wifi ? "wifi" : "automatic";
        std::string text = "rotation=" + std::to_string(quarterTurns) + "\nconnection=" + mode +
                           "\npairing-code=" + link.pairingCode + "\nphone-address=" + link.address + "\n";
        SDL_SaveFile(path().c_str(), text.data(), text.size());
    }

    void rotate(int turns) {
        quarterTurns = ((quarterTurns + turns) % 4 + 4) % 4;
        save();
    }

private:
    static std::string path() {
        char* folder = SDL_GetPrefPath("uipko", "BoardScope");
        std::string file = folder ? std::string(folder) + "settings.txt" : std::string();
        SDL_free(folder);
        return file;
    }
};

/// The connection fields being typed in the panel, applied when the user leaves a field.
struct ConnectionForm {
    char pairingCode[16] = {};
    char address[128] = {};

    explicit ConnectionForm(const PhoneLink::Options& options) {
        std::snprintf(pairingCode, sizeof pairingCode, "%s", options.pairingCode.c_str());
        std::snprintf(address, sizeof address, "%s", options.address.c_str());
    }
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

    void draw(SDL_FRect area, bool stale, int quarterTurns) {
        if (!texture_ || area.w <= 0 || area.h <= 0) return;
        // Fit the picture as it will look once turned; it turns about the target's center.
        bool sideways = quarterTurns % 2 == 1;
        float scale = std::min(area.w / (sideways ? height_ : width_), area.h / (sideways ? width_ : height_));
        float width = width_ * scale;
        float height = height_ * scale;
        SDL_FRect target{area.x + (area.w - width) / 2, area.y + (area.h - height) / 2, width, height};
        Uint8 shade = stale ? 90 : 255;
        SDL_SetTextureColorMod(texture_, shade, shade, shade);
        SDL_RenderTextureRotated(renderer_, texture_, nullptr, &target, 90.0 * quarterTurns, nullptr, SDL_FLIP_NONE);
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

/// Green on the charger, orange off it (the phone should stay plugged in), red when low.
ImVec4 batteryColor(const wire::State& state) {
    if (state.charging) return {0.35f, 0.85f, 0.4f, 1};
    if (state.battery >= 0 && state.battery < 20) return {1.0f, 0.3f, 0.3f, 1};
    return {1.0f, 0.6f, 0.2f, 1};
}

std::string batteryLabel(const wire::State& state) {
    std::string label = state.battery >= 0 ? std::to_string(state.battery) + "%" : "unknown";
    return state.charging ? label + ", charging" : label;
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
bool handleKey(const SDL_KeyboardEvent& key, Controls& controls, SDL_Window* window, Settings& settings, bool& showPanel) {
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
    case SDLK_R:
        if (!key.repeat) settings.rotate(shift ? -1 : 1);
        return true;
    case SDLK_L:
        if (!key.repeat && controls.state().hasTorch) controls.setLight(controls.light.value < 0.5f);
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

/// Returns true when the connection settings changed.
bool drawPanel(Controls& controls, Settings& settings, ConnectionForm& form, PhoneLink& link,
               const PhoneLink::Snapshot& snapshot, bool connected, float height) {
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

    ImGui::SeparatorText("Light");
    ImGui::BeginDisabled(!state.hasTorch);
    bool lightOn = controls.light.value >= 0.5f;
    if (ImGui::Button(lightOn ? "Light on" : "Light off", {ImGui::GetFrameHeight() * 4, 0})) controls.setLight(!lightOn);
    ImGui::SameLine();
    int brightness = static_cast<int>(std::lround(controls.lightLevel.value * 100));
    if (ImGui::SliderInt("##light", &brightness, 5, 100, "%d%%")) controls.setLightLevel(brightness / 100.0f);
    controls.lightLevel.held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Orientation");
    float turnWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    if (ImGui::Button("Rotate left", {turnWidth, 0})) settings.rotate(-1);
    ImGui::SameLine();
    if (ImGui::Button("Rotate right", {turnWidth, 0})) settings.rotate(1);
    ImGui::TextDisabled(settings.quarterTurns == 0 ? "As the phone sends it" : "Turned %d degrees right", settings.quarterTurns * 90);

    ImGui::SeparatorText("Connection");
    bool changed = false;
    static const char* modes[] = {"Automatic, USB first", "USB cable only", "Wi-Fi only"};
    int mode = static_cast<int>(settings.link.mode);
    if (ImGui::Combo("##connection", &mode, modes, IM_ARRAYSIZE(modes))) {
        settings.link.mode = static_cast<PhoneLink::Mode>(mode);
        changed = true;
    }
    if (settings.link.mode != PhoneLink::Mode::usb) {
        ImGui::TextDisabled("Wi-Fi pairing code");
        bool asked = snapshot.status == PhoneLink::Status::needsCode;
        if (asked) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.3f, 0.1f, 1));
        ImGui::InputTextWithHint("##code", "in BoardScope's Settings", form.pairingCode, sizeof form.pairingCode,
                                 ImGuiInputTextFlags_CharsDecimal);
        if (asked) ImGui::PopStyleColor();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            std::string digits;
            for (char c : std::string(form.pairingCode)) {
                if (c >= '0' && c <= '9') digits += c;
            }
            settings.link.pairingCode = digits;
            changed = true;
        }
        ImGui::TextDisabled("Phone address");
        ImGui::InputTextWithHint("##address", "found automatically", form.address, sizeof form.address,
                                 ImGuiInputTextFlags_CharsNoBlank);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            settings.link.address = form.address;
            changed = true;
        }
    }
    if (snapshot.status == PhoneLink::Status::takenOver && ImGui::Button("Take the iPhone back", {-1, 0})) link.reconnect();

    ImGui::SeparatorText("Phone");
    if (connected) {
        ImGui::Text("%s camera, light %s", state.telephoto ? "Telephoto" : "Main", state.torchOn ? "on" : "off");
        ImGui::Text("Heat:");
        ImGui::SameLine();
        ImGui::TextColored(heatColor(state.thermal), "%s", heatLabel(state));
        ImGui::Text("Battery:");
        ImGui::SameLine();
        ImGui::TextColored(batteryColor(state), "%s", batteryLabel(state).c_str());
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
    shortcut("L", "light on / off");
    shortcut("R / Shift+R", "rotate right / left");
    shortcut("Tab", "hide controls");
    shortcut("F11", "full screen");
    ImGui::PopItemWidth();
    ImGui::End();
    return changed;
}

void drawStatusLine(const PhoneLink::Snapshot& link, double shownFps, const FrameView& frames, float top) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, top});
    ImGui::SetNextWindowSize({io.DisplaySize.x, io.DisplaySize.y - top});
    ImGui::Begin("Status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    // The tunnel opens before the app answers; it only counts once the phone has said how it is.
    bool connected = link.status == PhoneLink::Status::connected && link.state;
    ImVec4 dot = connected ? ImVec4(0.35f, 0.85f, 0.4f, 1) : ImVec4(1.0f, 0.6f, 0.2f, 1);
    bool wifi = link.transport == PhoneLink::Transport::wifi;
    std::string where = wifi ? "Connected over Wi-Fi to " + link.phone : "Connected over USB";
    ImGui::TextColored(dot, "%s", connected ? where.c_str() : "Not connected");
    ImGui::SameLine();
    if (link.status == PhoneLink::Status::connected && !link.state) {
        ImGui::TextUnformatted("Waiting for BoardScope to answer...");
    } else if (!connected) {
        ImGui::TextUnformatted(link.problem.c_str());
    } else {
        std::string line = "   " + std::to_string(static_cast<int>(std::lround(shownFps))) + " fps";
        if (!frames.empty()) line += "   " + std::to_string(frames.width()) + "x" + std::to_string(frames.height());
        line += "   " + std::to_string(link.frameBytes / 1024) + " KB/frame";
        char roundTrip[48] = "   round trip --";
        if (link.roundTripMs >= 0) std::snprintf(roundTrip, sizeof roundTrip, "   round trip %.1f ms", link.roundTripMs);
        line += roundTrip;
        ImGui::TextUnformatted(line.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted("   heat");
        ImGui::SameLine();
        ImGui::TextColored(heatColor(link.state->thermal), "%s", heatLabel(*link.state));
        ImGui::SameLine();
        ImGui::TextUnformatted("   battery");
        ImGui::SameLine();
        ImGui::TextColored(batteryColor(*link.state), "%s", batteryLabel(*link.state).c_str());
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

    Settings settings;
    settings.load();
    PhoneLink link(settings.link);
    Controls controls(link);
    ConnectionForm form(settings.link);
    FrameView frames(renderer);
    bool showPanel = true;
    std::vector<std::uint8_t> jpeg;
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
                if (!io.WantTextInput && (connected || event.key.key == SDLK_TAB || event.key.key == SDLK_R ||
                                          event.key.key == SDLK_F11 || event.key.key == SDLK_ESCAPE)) {
                    handleKey(event.key, controls, window, settings, showPanel);
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

        if (link.takeFrame(jpeg) && frames.show(jpeg)) ++shownFrames;
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
        // The pairing code is typed in the panel, so it opens when the phone asks for one.
        if (snapshot.status == PhoneLink::Status::needsCode) showPanel = true;
        float imageWidth = io.DisplaySize.x - (showPanel ? panelWidth * style.FontScaleDpi : 0);
        if (showPanel && drawPanel(controls, settings, form, link, snapshot, connected, statusTop)) {
            settings.save();
            link.setOptions(settings.link);
        }
        drawStatusLine(snapshot, shownFps, frames, statusTop);

        bool stale = snapshot.status != PhoneLink::Status::connected;
        const char* message = nullptr;
        if (stale) {
            message = snapshot.problem.empty() ? "Looking for the iPhone..." : snapshot.problem.c_str();
        } else if (frames.empty()) {
            message = "Waiting for the first picture...";
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
        frames.draw({0, 0, imageWidth, statusTop}, stale, settings.quarterTurns);
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
