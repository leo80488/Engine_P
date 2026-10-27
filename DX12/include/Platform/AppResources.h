#pragma once
// Platform/AppResources.h - resource IDs shared by the exes' resource script (src/Platform/App.rc,
// compiled into Game.exe and Editor.exe through Engine.App.props) and the engine code that loads them
// from the exe module.
//
// Plain #defines only: rc.exe preprocesses this file too and understands nothing else. An exe built
// without App.rc has no such resource; LoadIcon then answers nullptr and the window gets the default
// icon.

#define DX12ENGINE_IDI_APP 101
