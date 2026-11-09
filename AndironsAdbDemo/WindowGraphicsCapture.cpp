#include "WindowGraphicsCapture.h"

#include <d3d11.h>
#include <dxgi.h>
#include <winrt/Windows.Foundation.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <QMutex>
#include <QMutexLocker>

#include <atomic>
#include <cstring>

using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

struct WindowGraphicsCapture::State
{
    HWND hwnd = nullptr;
    std::atomic_bool stopped{false};
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    winrt::event_token frameToken{};
    mutable QMutex imageMutex;
    QImage image;
};

WindowGraphicsCapture::WindowGraphicsCapture() = default;
WindowGraphicsCapture::~WindowGraphicsCapture() { stop(); }

bool WindowGraphicsCapture::start(HWND hwnd, QString& error)
{
    stop();
    error.clear();
    if (!hwnd || !IsWindow(hwnd))
    {
        error = QStringLiteral("目标窗口句柄无效");
        return false;
    }

    try
    {
        try { winrt::init_apartment(); }
        catch (const winrt::hresult_error& exception)
        {
            if (exception.code() != RPC_E_CHANGED_MODE) throw;
        }
        if (!GraphicsCaptureSession::IsSupported())
        {
            error = QStringLiteral("此 Windows 版本不支持无遮挡窗口捕获");
            return false;
        }

        auto state = std::make_shared<State>();
        state->hwnd = hwnd;
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, nullptr, 0, D3D11_SDK_VERSION, state->device.put(), nullptr, state->context.put()));
        auto dxgiDevice = state->device.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
        auto direct3DDevice = inspectable.as<IDirect3DDevice>();

        auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(factory->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(),
            winrt::put_abi(item)));
        state->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(direct3DDevice,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
        state->session = state->pool.CreateCaptureSession(item);
        state->session.IsCursorCaptureEnabled(false);
        state->frameToken = state->pool.FrameArrived([state](const auto& sender, const auto&) {
            if (state->stopped.load()) return;
            try
            {
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;
                auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::com_ptr<ID3D11Texture2D> source;
                winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), source.put_void()));
                D3D11_TEXTURE2D_DESC description{};
                source->GetDesc(&description);
                description.Usage = D3D11_USAGE_STAGING;
                description.BindFlags = 0;
                description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                description.MiscFlags = 0;
                winrt::com_ptr<ID3D11Texture2D> staging;
                winrt::check_hresult(state->device->CreateTexture2D(&description, nullptr, staging.put()));
                state->context->CopyResource(staging.get(), source.get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                winrt::check_hresult(state->context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
                QImage image(static_cast<int>(description.Width), static_cast<int>(description.Height), QImage::Format_ARGB32);
                if (!image.isNull())
                {
                    for (int y = 0; y < image.height(); ++y)
                        std::memcpy(image.scanLine(y), static_cast<const char*>(mapped.pData) + y * mapped.RowPitch,
                            static_cast<size_t>(image.bytesPerLine()));
                }
                state->context->Unmap(staging.get(), 0);
                if (image.isNull() || state->stopped.load()) return;
                QMutexLocker lock(&state->imageMutex);
                state->image = std::move(image);
            }
            catch (const winrt::hresult_error&)
            {
                // A single failed frame should not end an otherwise healthy capture session.
            }
        });
        state->session.StartCapture();
        state_ = std::move(state);
        return true;
    }
    catch (const winrt::hresult_error& exception)
    {
        error = QStringLiteral("无遮挡窗口捕获不可用（HRESULT 0x%1）")
            .arg(static_cast<quint32>(exception.code().value), 8, 16, QLatin1Char('0'));
        return false;
    }
}

void WindowGraphicsCapture::stop()
{
    auto state = std::move(state_);
    if (!state) return;
    state->stopped.store(true);
    try
    {
        if (state->pool) state->pool.FrameArrived(state->frameToken);
        if (state->session) state->session.Close();
        if (state->pool) state->pool.Close();
    }
    catch (const winrt::hresult_error&)
    {
    }
}

bool WindowGraphicsCapture::latest(QImage& image) const
{
    if (!state_) return false;
    QMutexLocker lock(&state_->imageMutex);
    image = state_->image;
    return !image.isNull();
}

HWND WindowGraphicsCapture::target() const
{
    return state_ ? state_->hwnd : nullptr;
}
