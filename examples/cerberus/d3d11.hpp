// Copyright (C) 2010-2015 Joshua Boyce
// See the file COPYING for copying permission.

#pragma once

#include <utility>
#include <functional>

#include <windows.h>

#include <d3d11.h>

#include "callbacks.hpp"

namespace hadesmem
{
namespace cerberus
{
typedef void OnReleaseD3D11Callback(ID3D11Device* device);

Callbacks<OnReleaseD3D11Callback>& GetOnReleaseD3D11Callbacks();

class D3D11Interface
{
public:
  virtual ~D3D11Interface()
  {
  }

  virtual std::size_t RegisterOnRelease(
    std::function<OnReleaseD3D11Callback> const& callback) = 0;

  virtual void UnregisterOnRelease(std::size_t id) = 0;
};

D3D11Interface& GetD3D11Interface() HADESMEM_DETAIL_NOEXCEPT;

void InitializeD3D11();

void DetourD3D11(HMODULE base);

void UndetourD3D11(bool remove);
}
}
