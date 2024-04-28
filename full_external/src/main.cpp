#include <Windows.h>
#include <iostream>
#include <dwmapi.h>
#include <d3d11.h>
#include <TlHelp32.h>
#include <string>

#include <imgui/imgui.h>
#include <imgui/imgui_impl_dx11.h>
#include <imgui/imgui_impl_win32.h>



//constexpr bool KERNEL = true;
//
//#define READ_MEM(handle,addr) \
//    if constexpr (KERNEL) { \
//        driver::read_memory(handle,addr); \
//    } else { \
//        read(handle,addr); \
//    }


namespace driver {
    namespace codes {
        // Used to setup the driver.
        constexpr ULONG attach =
            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x696, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

        // Read process memory.
        constexpr ULONG read =
            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x697, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

        // Read process memory.
        constexpr ULONG write =
            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x698, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
    }  // namespace codes

    // Shared between user mode & kernel mode.
    struct Request {
        HANDLE process_id;

        PVOID target;
        PVOID buffer;

        SIZE_T size;
        SIZE_T return_size;
    };

    bool attach_to_process(HANDLE driver_handle, const DWORD pid) {
        Request r;
        r.process_id = reinterpret_cast<HANDLE>(pid);

        return DeviceIoControl(driver_handle, codes::attach, &r, sizeof(r), &r, sizeof(r), nullptr,
            nullptr);
    }

    template <class T>
    T read_memory(HANDLE driver_handle, const std::uintptr_t addr) {
        T temp = {};

        Request r;
        r.target = reinterpret_cast<PVOID>(addr);
        r.buffer = &temp;
        r.size = sizeof(T);

        DeviceIoControl(driver_handle, codes::read, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr);

        return temp;
    }

    template <class T>
    void write_memory(HANDLE driver_handle, const std::uintptr_t addr, const T& value) {
        Request r;
        r.target = reinterpret_cast<PVOID>(addr);
        r.buffer = (PVOID)&value;
        r.size = sizeof(T);

        DeviceIoControl(driver_handle, codes::write, &r, sizeof(r), &r, sizeof(r), nullptr,
            nullptr);
    }
}  // namespace driver

static DWORD get_process_id(const wchar_t* process_name) {
    DWORD process_id = 0;

    HANDLE snap_shot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    if (snap_shot == INVALID_HANDLE_VALUE)
        return process_id;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(decltype(entry));

    if (Process32FirstW(snap_shot, &entry) == TRUE) {
        // Check if the first handle is the one we want.
        if (_wcsicmp(process_name, entry.szExeFile) == 0)
            process_id = entry.th32ProcessID;
        else {
            while (Process32NextW(snap_shot, &entry) == TRUE) {
                if (_wcsicmp(process_name, entry.szExeFile) == 0) {
                    process_id = entry.th32ProcessID;
                    break;
                }
            }
        }
    }

    CloseHandle(snap_shot);

    return process_id;
}

static std::uintptr_t get_module_base(const DWORD pid, const wchar_t* module_name) {
    std::uintptr_t module_base = 0;

    // Snap-shot of process' modules (dlls).
    HANDLE snap_shot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap_shot == INVALID_HANDLE_VALUE)
        return module_base;

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(decltype(entry));

    if (Module32FirstW(snap_shot, &entry) == TRUE) {
        if (wcsstr(module_name, entry.szModule) != nullptr)
            module_base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
        else {
            while (Module32NextW(snap_shot, &entry) == TRUE) {
                if (wcsstr(module_name, entry.szModule) != nullptr) {
                    module_base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                    break;
                }
            }
        }
    }

    CloseHandle(snap_shot);

    return module_base;
}

namespace offsets {
    constexpr auto dwLocalPlayerPawn = 0x173A3B8; // quale  offset? dwLocalPlayerController = 0x1915C08;
    constexpr auto dwEntityList = 0x18C6268;
    constexpr auto dwViewMatrix = 0x19278A0;

    constexpr auto bone_matrix = 0x00AA8; //????
    constexpr auto m_vecViewOffset = 0xC58;
    constexpr auto m_hPlayerPawn = 0x7E4;
    constexpr auto m_iHealth = 0x334;
    constexpr auto m_iTeamNum = 0x3CB; // uint8
    constexpr auto m_lifeState = 0x338; // int32
    constexpr auto m_vecOrigin = 0x80; // CNetworkOriginCellCoordQuantizedVector
    constexpr auto m_vOldOrigin = 0x127C;
    constexpr auto m_bDormant = 0xE7; // bool
}

struct Vector {
    Vector() noexcept = default;
    Vector(float x, float y, float z) noexcept : x(x), y(y), z(z) {}

    Vector& operator+=(const Vector& v) noexcept {
        x += v.x;
        y += v.y;
        z += v.z;
        return *this;
    }

    Vector& operator-=(const Vector& v) noexcept {
        x -= v.x;
        y -= v.y;
        z -= v.z;
        return *this;
    }

    Vector& operator*=(float s) noexcept {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }

    Vector& operator/=(float s) noexcept {
        x /= s;
        y /= s;
        z /= s;
        return *this;
    }



    float x, y, z;
};

Vector add(const Vector& v1,const Vector& v2) noexcept {
	Vector v3;
	v3.x = v1.x + v2.x;
	v3.y = v1.y + v2.y;
	v3.z = v1.z + v2.z;
    return v3;
}


struct ViewMatrix {
    ViewMatrix() noexcept = default;

    float* operator[](std::size_t i) noexcept {
        return matrix[i];
    }

    const float* operator[](std::size_t i) const noexcept {
        return matrix[i];
    }

    float matrix[4][4];
};

static bool world_to_screen(const Vector& origin, Vector& screen, const ViewMatrix& matrix) noexcept {
    auto w = matrix[3][0] * origin.x + matrix[3][1] * origin.y + matrix[3][2] * origin.z + matrix[3][3];
    if (w < 0.001f)
        return false;

    const float x = matrix[0][0] * origin.x + matrix[0][1] * origin.y + matrix[0][2] * origin.z + matrix[0][3];
    const float y = matrix[1][0] * origin.x + matrix[1][1] * origin.y + matrix[1][2] * origin.z + matrix[1][3];

    w = 1.0f / w;
    float nx = x * w;
    float ny = y * w;

    const ImVec2 size = ImGui::GetIO().DisplaySize;

    screen.x = (size.x / 2.0f * nx) + (nx + size.x / 2.0f);
    screen.y = -(size.y / 2.0f * ny) + (ny + size.y / 2.0f);

    return true;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 0L;

    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0L;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

template <typename T>
constexpr const T read(HANDLE handle, const std::uintptr_t& address)
{
    T value = { };
    ::ReadProcessMemory(handle, reinterpret_cast<const void*>(address), &value, sizeof(T), NULL);
    return value;
}



INT APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, INT nCmdShow) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"Overlay";

    RegisterClassExW(&wc);

    const HWND hWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST, wc.lpszClassName, L"Overlay", WS_POPUP, 0, 0, 1920, 1080, nullptr, nullptr, hInstance, nullptr);

    SetLayeredWindowAttributes(hWnd, RGB(0, 0, 0), BYTE(255), LWA_ALPHA);

    {
        RECT client_area{};
        GetClientRect(hWnd, &client_area);

        RECT window_area{};
        GetWindowRect(hWnd, &window_area);

        POINT diff{};
        ClientToScreen(hWnd, &diff);

        const MARGINS margins{
            window_area.left + (diff.x - window_area.left),
            window_area.top + (diff.y - window_area.top),
            window_area.right,
            window_area.bottom
        };

        DwmExtendFrameIntoClientArea(hWnd, &margins);
    }

    DXGI_SWAP_CHAIN_DESC sdc{};
    sdc.BufferDesc.RefreshRate.Numerator = 60; // refresh rate
    sdc.BufferDesc.RefreshRate.Denominator = 1;
    sdc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sdc.SampleDesc.Count = 1U;
    sdc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sdc.BufferCount = 2U;
    sdc.OutputWindow = hWnd;
    sdc.Windowed = TRUE;
    sdc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sdc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    constexpr D3D_FEATURE_LEVEL feature_levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    ID3D11Device* device = nullptr; 
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* render_target_view = nullptr;
    D3D_FEATURE_LEVEL feature_level{};

    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0U, feature_levels, 2U, D3D11_SDK_VERSION, &sdc, &swap_chain, &device, &feature_level, &context);

    ID3D11Texture2D* back_buffer = nullptr;
    swap_chain->GetBuffer(0U, IID_PPV_ARGS(&back_buffer));

    if (back_buffer) {
        device->CreateRenderTargetView(back_buffer, nullptr, &render_target_view);
        back_buffer->Release();
    }
    else {
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX11_Init(device, context);


    //GUI FINISHED, STARTING GETTING PROCESS
    const DWORD pid = get_process_id(L"cs2.exe");

    if (pid == 0) {
        std::cout << "Failed to find cs2.\n";
        std::cin.get();
        return 1;
    }

    const HANDLE driver = CreateFile(L"\\\\.\\driver_peso", GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    //const HANDLE pHandle = OpenProcess(PROCESS_ALL_ACCESS, TRUE, pid);

    if (driver == INVALID_HANDLE_VALUE) {
        std::cout << "Failed to create our driver handle.\n";
        //std::cin.get();
        return 1;
    }

    if (driver::attach_to_process(driver, pid) == true) {
        std::cout << "Attachment successful.\n";
    }
    const std::uintptr_t client = get_module_base(pid, L"client.dll");
    if (client != 0) {
        std::cout << "Client found.\n";
    }
    // main application loop
    bool running = true;
    while (running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);

            if (msg.message == WM_QUIT) {
                running = false;
            }
        }

        if (!running) {
            break;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        ImGui::NewFrame();





         //cheat logic here
        //const auto local_player = read<std::uintptr_t>(pHandle, client + offsets::dwLocalPlayerPawn);
		const auto local_player = driver::read_memory<std::uintptr_t>(driver, client + offsets::dwLocalPlayerPawn);

        //const auto entity_list = read<std::uintptr_t>(pHandle, client + offsets::dwEntityList);
		const auto entity_list = driver::read_memory<std::uintptr_t>(driver, client + offsets::dwEntityList);
        printf("entity_list: %p\n", entity_list);

        //const auto list_entry = read<std::uintptr_t>(pHandle, entity_list + 0x10);
		const auto list_entry = driver::read_memory<std::uintptr_t>(driver, entity_list + 0x10);
        //const auto view_matrix = read<ViewMatrix>(pHandle, client + offsets::dwViewMatrix);
		const auto view_matrix = driver::read_memory<ViewMatrix>(driver, client + offsets::dwViewMatrix);
        //per testare la gui
        //ImGui::GetBackgroundDrawList()->AddCircleFilled(ImVec2(500.0f, 500.0f), 50.0f, IM_COL32(255, 0, 0, 255), 32);

        printf("prima del mega loop schifoso\n");
        if (local_player) {
            //const auto local_player_team = read<int>(pHandle, local_player + offsets::m_iTeamNum);
			const auto local_player_team = driver::read_memory<int>(driver, local_player + offsets::m_iTeamNum);
			printf("local_player_team: %d\n", local_player_team);

            for (int i = 0; i < 64; i++) {

				//const auto current_controller = read<std::uintptr_t>(pHandle, list_entry + i * 0x78);
				const auto current_controller = driver::read_memory<std::uintptr_t>(driver, list_entry + i * 0x78);
                if (current_controller == 0) {
                    continue;
                }

				printf("current_controller: %p\n", current_controller);

                //const auto pawn_handle = read<std::uintptr_t>(pHandle, current_controller + offsets::m_hPlayerPawn);
				const auto pawn_handle = driver::read_memory<std::uintptr_t>(driver, current_controller + offsets::m_hPlayerPawn);

				printf("pawn_handle: %p\n", pawn_handle);
                if (pawn_handle == 0) {
                    continue;
                }


                //const auto list_entry2 = read<std::uintptr_t>(pHandle, entity_list + 0x8 * ((pawn_handle & 0x7FFF) >> 9) + 0x10);
				const auto list_entry2 = driver::read_memory<std::uintptr_t>(driver, entity_list + 0x8 * ((pawn_handle & 0x7FFF) >> 9) + 0x10);

                //const auto current_pawn = read<std::uintptr_t>(pHandle, list_entry2 + 0x78 * (pawn_handle & 0x1FF));
				const auto current_pawn = driver::read_memory<std::uintptr_t>(driver, list_entry2 + 0x78 * (pawn_handle & 0x1FF));

                //const auto health = read<int>(pHandle, current_pawn + offsets::m_iHealth);
				const auto health = driver::read_memory<int>(driver, current_pawn + offsets::m_iHealth);

				if (health <= 0) {
					continue;
				}

				//int life_state = read<int>(pHandle, current_pawn + offsets::m_lifeState);
				int life_state = driver::read_memory<int>(driver, current_pawn + offsets::m_lifeState); 


				if (life_state != 256) {
					continue;
				}
				//printf("health: %d\n", health);

				//const auto dormant = read<bool>(pHandle, current_pawn + offsets::m_bDormant);
				const auto dormant = driver::read_memory<bool>(driver, current_pawn + offsets::m_bDormant);
				if (dormant) {
					continue;
				}

                //const auto entity_origin = read<Vector>(pHandle, current_pawn + offsets::m_vOldOrigin);
				const auto entity_origin = driver::read_memory<Vector>(driver, current_pawn + offsets::m_vOldOrigin);

				//const auto team_num = read<int>(pHandle, current_pawn + offsets::m_iTeamNum);
				const auto team_num = driver::read_memory<int>(driver, current_pawn + offsets::m_iTeamNum);
				if (team_num == local_player_team) {
					continue;
				}
				//const auto team_num = driver::read_memory<int>(driver, current_pawn + offsets::m_iTeamNum);
				//const auto view_offset = read<Vector>(pHandle, current_pawn + offsets::m_vecViewOffset);
				const auto view_offset = driver::read_memory<Vector>(driver, current_pawn + offsets::m_vecViewOffset);



                Vector top;
                Vector bottom;
                world_to_screen(entity_origin,bottom , view_matrix);

                const Vector v1 = { 0,0,85 };
				Vector sum = add(entity_origin, v1);
				world_to_screen(sum, top, view_matrix);

                const auto height = bottom.y - top.y;
                const auto width = height * 0.35f;


				uint8_t red = 255 - (health * 2.55f)*0.5;
				uint8_t green = health * 2.55f;
                
				float length = height * health / 100;
                

                ImGui::GetBackgroundDrawList()->AddRect(ImVec2(top.x - width, top.y), ImVec2(top.x + width, top.y + height), IM_COL32(0, 0, 0, 255), 0.0, 0, 2.0f);

				ImGui::GetBackgroundDrawList()->AddLine(ImVec2(top.x - width, bottom.y), ImVec2(top.x - width, bottom.y - length), IM_COL32(red, green, 0, 255), 2.0f);
				ImGui::GetBackgroundDrawList()->AddText(ImVec2(top.x - width/2, top.y -10), IM_COL32(255, 255, 255, 255), std::to_string(health).c_str());

            }
        }

        // render here
        ImGui::Render();

        constexpr float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetRenderTargets(1, &render_target_view, nullptr);
        context->ClearRenderTargetView(render_target_view, clear_color);

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        swap_chain->Present(1, 0);
    }

    // cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();

    ImGui::DestroyContext();

    if (render_target_view) {
        render_target_view->Release();
    }

    if (swap_chain) {
        swap_chain->Release();
    }

    if (context) {
        context->Release();
    }

    if (device) {
        device->Release();
    }

    DestroyWindow(hWnd);
    UnregisterClassW(wc.lpszClassName, hInstance);


    return 0;
}