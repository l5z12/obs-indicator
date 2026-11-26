// SPDX-License-Identifier: GPL-3.0-or-later
// Tiny WIC helpers shared by the developer tools.
#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>

namespace tools {

using Microsoft::WRL::ComPtr;

inline ComPtr<IWICImagingFactory> Wic()
{
	ComPtr<IWICImagingFactory> f;
	CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
	return f;
}

inline bool SavePng(IWICBitmapSource *src, const std::wstring &path)
{
	auto wic = Wic();
	if (!wic)
		return false;
	ComPtr<IWICStream> stream;
	ComPtr<IWICBitmapEncoder> enc;
	ComPtr<IWICBitmapFrameEncode> frame;
	if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
	    FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
	    FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
	    FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)))
		return false;
	ComPtr<IWICBitmapSource> straight;
	if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, src, &straight)))
		return false;
	UINT w = 0, h = 0;
	straight->GetSize(&w, &h);
	WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
	frame->SetSize(w, h);
	frame->SetPixelFormat(&fmt);
	return SUCCEEDED(frame->WriteSource(straight.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
	       SUCCEEDED(enc->Commit());
}

// Saves a top-down 32bpp BGRX/BGRA buffer, forcing alpha to opaque.
inline bool SavePng(void *bits, int w, int h, const std::wstring &path)
{
	auto *px = static_cast<BYTE *>(bits);
	for (int i = 0; i < w * h; i++)
		px[i * 4 + 3] = 255;
	auto wic = Wic();
	ComPtr<IWICBitmap> bmp;
	if (!wic || FAILED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, w * h * 4, px, &bmp)))
		return false;
	return SavePng(bmp.Get(), path);
}

// Captures a screen rectangle including layered windows (CAPTUREBLT).
inline bool SaveScreenRect(const RECT &r, const std::wstring &path)
{
	const int w = r.right - r.left, h = r.bottom - r.top;
	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	BITMAPINFO bi{};
	bi.bmiHeader = {sizeof(bi.bmiHeader), w, -h, 1, 32, BI_RGB};
	void *bits = nullptr;
	HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	HGDIOBJ old = SelectObject(mem, bmp);
	BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
	GdiFlush();
	const bool ok = SavePng(bits, w, h, path);
	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
	return ok;
}

// Captures one window (including its frame) via PrintWindow.
inline bool SaveWindow(HWND hwnd, const std::wstring &path)
{
	RECT r;
	GetWindowRect(hwnd, &r);
	const int w = r.right - r.left, h = r.bottom - r.top;
	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	BITMAPINFO bi{};
	bi.bmiHeader = {sizeof(bi.bmiHeader), w, -h, 1, 32, BI_RGB};
	void *bits = nullptr;
	HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	HGDIOBJ old = SelectObject(mem, bmp);
	PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
	GdiFlush();
	const bool ok = SavePng(bits, w, h, path);
	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
	return ok;
}

} // namespace tools
