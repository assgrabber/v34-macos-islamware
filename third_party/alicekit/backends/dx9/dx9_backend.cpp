#include "dx9_backend.hpp"

#include "blur/blur.hpp"
#include "grayscale/grayscale.hpp"

#include <glow/glow.hpp>
#include <model_engine/model_engine.hpp>

using alice::c_dx9_backend;

c_dx9_backend::c_dx9_backend( IDirect3DDevice9* dev, int w, int h )
    : device( dev )
    , width( w )
    , height( h )
{
}

void c_dx9_backend::set_size( int w, int h )
{
    width = w;
    height = h;
}

std::unique_ptr< alice::c_blur > c_dx9_backend::make_blur( )
{
    return std::make_unique< c_dx9_blur >( device, width, height );
}

std::unique_ptr< alice::c_glow > c_dx9_backend::make_glow( )
{
    return nullptr;
}

std::unique_ptr< alice::c_grayscale > c_dx9_backend::make_grayscale( )
{
    return std::make_unique< c_dx9_grayscale >( device );
}

std::unique_ptr< alice::c_model_engine > c_dx9_backend::make_model_engine( )
{
    return nullptr;
}

ImTextureID c_dx9_backend::create_texture_rgba( void const* rgba, int w, int h )
{
    if ( !device || !rgba || w <= 0 || h <= 0 )
        return 0;

    // DXVK rejects D3DPOOL_MANAGED (D3DERR_INVALIDCALL), so use a dynamic
    // default-pool texture like ImGui's own DX9 backend does.  These do not
    // survive a device Reset; alice_ui rebuilds the atlas there.
    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = device->CreateTexture( w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr );
    if ( FAILED( hr ) || !tex )
        return 0;

    D3DLOCKED_RECT locked { };
    hr = tex->LockRect( 0, &locked, nullptr, D3DLOCK_DISCARD );
    if ( FAILED( hr ) )
    {
        tex->Release( );
        return 0;
    }

    auto const* src = static_cast< uint8_t const* >( rgba );
    auto* dst = static_cast< uint8_t* >( locked.pBits );
    int const row_bytes = w * 4;

    for ( int y = 0; y < h; y++ )
        memcpy( dst + ( size_t ) y * locked.Pitch, src + ( size_t ) y * row_bytes, row_bytes );

    tex->UnlockRect( 0 );
    return reinterpret_cast< ImTextureID >( tex );
}

void c_dx9_backend::destroy_texture( ImTextureID id )
{
    if ( auto* tex = reinterpret_cast< IDirect3DTexture9* >( id ) )
        tex->Release( );
}
