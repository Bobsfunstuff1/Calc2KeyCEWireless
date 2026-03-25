#include <SDL2/SDL.h>
#include <GL/glew.h>
#include <imgui.h>
#include <imgui/backends/imgui_impl_sdl2.h>
#include <imgui/backends/imgui_impl_opengl3.h>
#include <X11/keysym.h>

#include <libusb-1.0/libusb.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "bridge_client.h"
#include "receiver.h"
#include "sender.h"
#include "keypad.h"

#define IMGUI_IMPL_OPENGL_ES2

std::atomic<bool> stop(false);
std::atomic<bool> done(false);

bool bindingKeyState = false;
uint8_t bindingKey = static_cast<uint8_t>(-1);
uint32_t newBinding = 0;
static uint32_t tempBinding = 0;
static bool waitingForFirstKey = false;

bool calcConnected = false;
bool bridgeMode = false;
std::string bridgeHost;
uint16_t bridgePort = 28400;
std::atomic<bool> bridgeConnected(false);
std::string bridgeStatus = "Bridge disabled";

libusb_device_handle* devHandle = nullptr;

const char* getCalcKeyName(uint8_t i) {
    const uint8_t dataIdx = (i >> 4) & 7;
    const uint8_t keyShift = i & 0xF;
    const uint8_t key = 1 << keyShift;
    switch (dataIdx) {
    case 0:
        if (key & calc_Graph) return "Graph";
        if (key & calc_Trace) return "Trace";
        if (key & calc_Zoom) return "Zoom";
        if (key & calc_Window) return "Window";
        if (key & calc_Yequ) return "Yequ";
        if (key & calc_Second) return "Second";
        if (key & calc_Mode) return "Mode";
        if (key & calc_Del) return "Del";
        break;
    case 6:
        if (key & calc_Down) return "Down";
        if (key & calc_Left) return "Left";
        if (key & calc_Right) return "Right";
        if (key & calc_Up) return "Up";
        break;
    default:
        break;
    }
    return "Unknown";
}

static void drawKeyboardKeyName(const char* format, uint16_t keyCode) {
    char buf[64];
    if ((keyCode >= 0x30 && keyCode <= 0x39) || (keyCode >= 0x41 && keyCode <= 0x5A)) {
        std::snprintf(buf, sizeof(buf), "%c", keyCode);
    }
    else {
        std::snprintf(buf, sizeof(buf), "Key Code: 0x%x", keyCode);
    }
    ImGui::Text(format, buf);
}

static void drawKeyBindings() {
    if (bridgeMode) {
        ImGui::TextWrapped("Bridge mode is active. Calculator key packets are forwarded over TCP, so local X11 key bindings are not used in this mode.");
        ImGui::Separator();
    }

    if (ImGui::BeginTable("table", 2)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 0);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        if (!bridgeMode && ImGui::Button("Bind key")) {
            lastCalcKeyUp = static_cast<uint8_t>(-1);
            bindingKeyState = true;
            bindingKey = static_cast<uint8_t>(-1);
            waitingForFirstKey = true;
            tempBinding = 0;
        }

        if (bindingKeyState) {
            if (bindingKey == static_cast<uint8_t>(-1)) {
                ImGui::Text("Press a calculator key to bind");
            }
            else {
                ImGui::Text("Binding Key: %s", getCalcKeyName(bindingKey));

                if (tempBinding == 0) {
                    tempBinding = keyBindings[bindingKey] ? keyBindings[bindingKey] : 0x10000;
                }

                ImGui::Text("Input Type:");
                static bool bindingAsKeyboard = true;

                if (ImGui::RadioButton("Keyboard", bindingAsKeyboard)) {
                    bindingAsKeyboard = true;
                    if ((tempBinding & 0x10000) == 0) {
                        tempBinding = 0x10000;
                    }
                }

                if (ImGui::RadioButton("Mouse", !bindingAsKeyboard)) {
                    bindingAsKeyboard = false;
                    if (tempBinding & 0x10000) {
                        tempBinding = CUSTOM_MOUSE_MOVE | (0 << 8);
                    }
                }

                const bool isKeyboard = bindingAsKeyboard;
                if (isKeyboard) {
                    if (!(tempBinding & 0xFFFF)) {
                        ImGui::Text("Press a keyboard key to bind...");
                    }
                    else {
                        drawKeyboardKeyName("Selected Key: %s", static_cast<uint16_t>(tempBinding));
                    }
                }
                else {
                    ImGui::Separator();
                    if (ImGui::RadioButton("Move Up", tempBinding == (CUSTOM_MOUSE_MOVE | (0 << 8)))) tempBinding = CUSTOM_MOUSE_MOVE | (0 << 8);
                    if (ImGui::RadioButton("Move Down", tempBinding == (CUSTOM_MOUSE_MOVE | (1 << 8)))) tempBinding = CUSTOM_MOUSE_MOVE | (1 << 8);
                    if (ImGui::RadioButton("Move Left", tempBinding == (CUSTOM_MOUSE_MOVE | (2 << 8)))) tempBinding = CUSTOM_MOUSE_MOVE | (2 << 8);
                    if (ImGui::RadioButton("Move Right", tempBinding == (CUSTOM_MOUSE_MOVE | (3 << 8)))) tempBinding = CUSTOM_MOUSE_MOVE | (3 << 8);
                    if (ImGui::RadioButton("Left Click", tempBinding == 1)) tempBinding = 1;
                    if (ImGui::RadioButton("Right Click", tempBinding == 3)) tempBinding = 3;
                }

                if (ImGui::Button("Bind")) {
                    if (tempBinding != 0) {
                        keyBindings[bindingKey] = tempBinding;
                    }
                    bindingKeyState = false;
                    waitingForFirstKey = false;
                }
            }

            if (ImGui::Button("Cancel")) {
                tempBinding = 0;
                bindingKeyState = false;
                waitingForFirstKey = false;
            }
        }
        else if (!bridgeMode) {
            if (ImGui::Button("Save Keybindings")) {
                std::ofstream file("keybinds.cfg", std::ios::binary);
                if (file) {
                    file.write(reinterpret_cast<char*>(keyBindings), sizeof(keyBindings));
                }
            }

            if (ImGui::Button("Load Keybindings")) {
                std::ifstream file("keybinds.cfg", std::ios::binary);
                if (file) {
                    file.read(reinterpret_cast<char*>(keyBindings), sizeof(keyBindings));
                }
            }
        }

        ImGui::TableSetColumnIndex(1);
        if (!bridgeMode && ImGui::BeginTable("boundTable", 3)) {
            ImGui::TableSetupColumn("Calc Key");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();

            for (uint8_t i = 0; i < 128; ++i) {
                if (!keyBindings[i]) {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", getCalcKeyName(i));

                if (keyBindings[i] & 0x10000) {
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("Keyboard");
                    ImGui::TableSetColumnIndex(2);
                    drawKeyboardKeyName("%s", keyBindings[i] & 0xFFFF);
                }
                else {
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("Mouse");
                    ImGui::TableSetColumnIndex(2);
                    const uint8_t action = keyBindings[i] & 0xFF;
                    const uint8_t dir = (keyBindings[i] >> 8) & 0xFF;

                    if (action == CUSTOM_MOUSE_MOVE && dir <= 3) {
                        const char* dirNames[] = { "Move Up", "Move Down", "Move Left", "Move Right" };
                        ImGui::Text("%s", dirNames[dir]);
                    }
                    else if (action == 1) {
                        ImGui::Text("Left Click");
                    }
                    else if (action == 3) {
                        ImGui::Text("Right Click");
                    }
                    else {
                        ImGui::Text("Mouse Action %d", action);
                    }
                }
            }

            ImGui::EndTable();
        }

        ImGui::EndTable();
    }

    if (!bridgeMode) {
        ImGui::SliderInt("Mouse Speed", &mouseSpeed, 1, 100);
        ImGui::SliderFloat("Red Mult", &redMult, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Green Mult", &greenMult, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Blue Mult", &blueMult, 0.0f, 3.0f, "%.2f");
    }
}

static void parseArgs(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--bridge" && i + 1 < argc) {
            bridgeMode = true;
            std::string value = argv[++i];
            const std::size_t colon = value.find(':');
            if (colon == std::string::npos) {
                bridgeHost = value;
            }
            else {
                bridgeHost = value.substr(0, colon);
                bridgePort = static_cast<uint16_t>(std::stoi(value.substr(colon + 1)));
            }
        }
        else if (arg == "--bridge-host" && i + 1 < argc) {
            bridgeMode = true;
            bridgeHost = argv[++i];
        }
        else if (arg == "--bridge-port" && i + 1 < argc) {
            bridgeMode = true;
            bridgePort = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
    }

    if (bridgeMode && bridgeHost.empty()) {
        bridgeHost = "127.0.0.1";
    }
}

static void runner() {
    libusb_init(nullptr);

    while (!done.load()) {
        devHandle = nullptr;
        while (!done.load() && !devHandle) {
            devHandle = libusb_open_device_with_vid_pid(nullptr, 0x0451, 0xE009);
            if (!devHandle) {
                calcConnected = false;
                bridgeConnected.store(false);
                SDL_Delay(100);
                continue;
            }

            calcConnected = true;
            libusb_set_auto_detach_kernel_driver(devHandle, 1);
            libusb_claim_interface(devHandle, 0);

            BridgeClient bridgeClient;
            BridgeClient* bridgePtr = nullptr;
            if (bridgeMode) {
                bridgeStatus = "Connecting to bridge host...";
                if (!bridgeClient.connectTo(bridgeHost, bridgePort)) {
                    bridgeStatus = "Bridge connect failed: " + bridgeClient.lastError();
                    bridgeConnected.store(false);
                    libusb_release_interface(devHandle, 0);
                    libusb_close(devHandle);
                    devHandle = nullptr;
                    calcConnected = false;
                    SDL_Delay(1000);
                    continue;
                }

                bridgeConnected.store(true);
                bridgeStatus = "Bridge connected to " + bridgeHost + ":" + std::to_string(bridgePort);
                bridgePtr = &bridgeClient;
            }
            else {
                bridgeStatus = "Bridge disabled";
            }

            std::thread tRecv([&]() { receiveThread(devHandle, stop, bridgeMode, bridgePtr); });
            std::thread tSend([&]() { sendThread(devHandle, stop, bridgeMode, bridgePtr); });

            tRecv.join();
            tSend.join();

            if (bridgePtr) {
                bridgePtr->close();
                bridgeConnected.store(false);
                bridgeStatus = "Bridge disconnected";
            }

            libusb_release_interface(devHandle, 0);
            libusb_close(devHandle);
            devHandle = nullptr;
            stop.store(false);
        }
    }

    libusb_exit(nullptr);
}

int main(int argc, char* argv[]) {
    parseArgs(argc, argv);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow("Calc2KeyCE", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    glewInit();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    std::thread usbThread(runner);

    SDL_Event event;
    while (!done.load()) {
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                done.store(true);
            }

            if (!bridgeMode && bindingKeyState && waitingForFirstKey && bindingKey != static_cast<uint8_t>(-1)) {
                if (event.type == SDL_KEYDOWN) {
                    const char* keyName = SDL_GetKeyName(event.key.keysym.sym);
                    const KeySym keysym = XStringToKeysym(keyName);
                    if (keysym != NoSymbol) {
                        tempBinding = 0x10000 | static_cast<uint32_t>(keysym);
                        waitingForFirstKey = false;
                    }
                }
                else if (event.type == SDL_MOUSEBUTTONDOWN) {
                    tempBinding = static_cast<uint32_t>(event.button.button);
                    waitingForFirstKey = false;
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Calc2KeyCE");
        ImGui::Text("Status: Calculator %s", calcConnected ? "connected" : "disconnected");
        ImGui::Text("Mode: %s", bridgeMode ? "TCP bridge relay" : "Direct host mode");
        if (bridgeMode) {
            ImGui::TextWrapped("Bridge: %s", bridgeStatus.c_str());
        }
        drawKeyBindings();
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    stop.store(true);
    usbThread.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
