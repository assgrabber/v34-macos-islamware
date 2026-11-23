#pragma once

struct IDirect3DDevice9;

namespace alice_ui
{
    void Init( IDirect3DDevice9* device, int width, int height );
    void Shutdown( );

    // D3D9 device lifecycle.  The blur uses default-pool targets that must go
    // away across Reset.
    void OnLostDevice( );
    void OnResetDevice( IDirect3DDevice9* device, int width, int height );

    // Called once per frame, before ImGui::NewFrame.  Captures the scene for
    // the backdrop blur.
    void PrepareFrame( );

    // Called between ImGui::NewFrame and ImGui::Render.
    void Render( );

    void Toggle( );
    void SetOpen( bool open );
    bool IsOpen( );
    bool IsBinding( );

    // Config file API for the alt (custom) menu.
    void ConfigMenuInit( );
    int ConfigMenuCount( );
    const char* ConfigMenuName( int index );
    void ConfigMenuSave( const char* name );
    void ConfigMenuLoad( const char* name );
    void ConfigMenuDelete( const char* name );
    void ConfigMenuRefresh( );
} // namespace alice_ui
