#include "blur.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>

using alice::c_dx9_blur;

namespace
{
    // number of halving steps; 5 → down to 1/32 which is plenty for a backdrop
    constexpr int k_levels = 5;

    struct quad_vertex_t
    {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
} // namespace

c_dx9_blur::c_dx9_blur( IDirect3DDevice9* dev, int w, int h )
    : device( dev )
{
    create_levels( w, h );
}

c_dx9_blur::~c_dx9_blur( )
{
    release( );
}

void c_dx9_blur::release( )
{
    for ( auto& lvl : levels )
    {
        if ( lvl.surf ) lvl.surf->Release( );
        if ( lvl.tex ) lvl.tex->Release( );
    }
    levels.clear( );
}

void c_dx9_blur::create_levels( int w, int h )
{
    width = w;
    height = h;

    if ( !device || w <= 0 || h <= 0 )
        return;

    int lw = w;
    int lh = h;

    for ( int i = 0; i < k_levels; i++ )
    {
        lw = std::max( 1, lw / 2 );
        lh = std::max( 1, lh / 2 );

        level_t lvl { };
        lvl.w = lw;
        lvl.h = lh;

        if ( FAILED( device->CreateTexture( lw, lh, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &lvl.tex, nullptr ) ) || !lvl.tex )
        {
            break;
        }

        lvl.tex->GetSurfaceLevel( 0, &lvl.surf );
        levels.push_back( lvl );

        if ( lw <= 4 || lh <= 4 )
            break;
    }
}

void c_dx9_blur::resize( int w, int h )
{
    if ( w == width && h == height )
        return;
    release( );
    create_levels( w, h );
    last_frame = -1;
    captured_this_frame = false;
}

ImTextureID c_dx9_blur::get_texture( ) const
{
    if ( levels.empty( ) )
        return 0;
    return reinterpret_cast< ImTextureID >( levels.front( ).tex );
}

void c_dx9_blur::prepare_frame( float, int )
{
    capture( );
}

void c_dx9_blur::begin( ImDrawList*, float, int )
{
    capture( );
}

void c_dx9_blur::uv_for( ImVec2 p_min, ImVec2 p_max, ImVec2& uv_min, ImVec2& uv_max ) const
{
    if ( width <= 0 || height <= 0 )
    {
        uv_min = ImVec2( 0, 0 );
        uv_max = ImVec2( 1, 1 );
        return;
    }

    uv_min = ImVec2( p_min.x / ( float ) width, p_min.y / ( float ) height );
    uv_max = ImVec2( p_max.x / ( float ) width, p_max.y / ( float ) height );
}

void c_dx9_blur::add_image( ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, ImU32 col ) const
{
    if ( !dl || levels.empty( ) )
        return;

    ImVec2 uv0, uv1;
    uv_for( p_min, p_max, uv0, uv1 );
    dl->AddImage( get_texture( ), p_min, p_max, uv0, uv1, col );
}

void c_dx9_blur::add_image_rounded( ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, float rounding,
                                    ImDrawFlags flags, ImU32 col ) const
{
    if ( !dl || levels.empty( ) )
        return;

    ImVec2 uv0, uv1;
    uv_for( p_min, p_max, uv0, uv1 );
    dl->AddImageRounded( get_texture( ), p_min, p_max, uv0, uv1, col, rounding, flags );
}

void c_dx9_blur::add_image_squircle( ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, float rounding,
                                     ImDrawFlags flags, float, float, int, ImU32 col ) const
{
    add_image_rounded( dl, p_min, p_max, rounding, flags, col );
}

void c_dx9_blur::apply( ImDrawList* dl, ImVec2 rect_min, ImVec2 rect_max,
                        float, float rounding, int, float alpha )
{
    capture( );
    add_image_rounded( dl, rect_min, rect_max, rounding, ImDrawFlags_RoundCornersAll,
                       IM_COL32( 255, 255, 255, ( int )( alpha * 255.f ) ) );
}

void c_dx9_blur::apply_squircle( ImDrawList* dl, ImVec2 rect_min, ImVec2 rect_max,
                                 float, float rounding, float, float, int, float alpha )
{
    capture( );
    add_image_rounded( dl, rect_min, rect_max, rounding, ImDrawFlags_RoundCornersAll,
                       IM_COL32( 255, 255, 255, ( int )( alpha * 255.f ) ) );
}

void c_dx9_blur::blend_level( level_t& dst, IDirect3DTexture9* src )
{
    if ( !device || !dst.surf || !src )
        return;

    IDirect3DStateBlock9* state = nullptr;
    device->CreateStateBlock( D3DSBT_ALL, &state );
    if ( state )
    {
        state->Capture( );
        state->Apply( );
    }

    device->SetRenderTarget( 0, dst.surf );
    device->SetDepthStencilSurface( nullptr );

    D3DVIEWPORT9 vp { 0, 0, ( DWORD ) dst.w, ( DWORD ) dst.h, 0.f, 1.f };
    device->SetViewport( &vp );

    device->SetPixelShader( nullptr );
    device->SetVertexShader( nullptr );
    device->SetFVF( D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 );

    device->SetTexture( 0, src );
    device->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_MODULATE );
    device->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
    device->SetTextureStageState( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
    device->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 );
    device->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE );
    device->SetTextureStageState( 1, D3DTSS_COLOROP, D3DTOP_DISABLE );
    device->SetTextureStageState( 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE );

    device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
    device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
    device->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
    device->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
    device->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );

    device->SetRenderState( D3DRS_ZENABLE, FALSE );
    device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
    device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
    device->SetRenderState( D3DRS_LIGHTING, FALSE );
    device->SetRenderState( D3DRS_FOGENABLE, FALSE );
    device->SetRenderState( D3DRS_STENCILENABLE, FALSE );
    device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
    device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
    device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
    device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
    device->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, FALSE );

    float const w = ( float ) dst.w;
    float const h = ( float ) dst.h;
    // 50% of the blurrier level is mixed on top of the sharper one, which
    // accumulates into a smooth kernel as we walk the pyramid back up.
    DWORD const tint = D3DCOLOR_ARGB( 128, 255, 255, 255 );
    quad_vertex_t verts[ 4 ] = {
        { 0.f, 0.f, 0.5f, 1.f, tint, 0.f, 0.f },
        { w,   0.f, 0.5f, 1.f, tint, 1.f, 0.f },
        { 0.f, h,   0.5f, 1.f, tint, 0.f, 1.f },
        { w,   h,   0.5f, 1.f, tint, 1.f, 1.f },
    };

    device->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, verts, sizeof( quad_vertex_t ) );

    if ( state )
    {
        state->Apply( );
        state->Release( );
    }
}

void c_dx9_blur::capture( )
{
    if ( !device || levels.empty( ) )
        return;

    int const frame = ImGui::GetFrameCount( );
    if ( captured_this_frame && frame == last_frame )
        return;
    last_frame = frame;
    captured_this_frame = true;

    IDirect3DSurface9* source = nullptr;
    if ( FAILED( device->GetRenderTarget( 0, &source ) ) || !source )
        return;

    // blend_level switches render targets; DXVK does not always restore them
    // through a state block, so save everything explicitly.
    IDirect3DSurface9* saved_rt = nullptr;
    IDirect3DSurface9* saved_ds = nullptr;
    D3DVIEWPORT9 saved_vp { };
    device->GetRenderTarget( 0, &saved_rt );
    device->GetDepthStencilSurface( &saved_ds );
    device->GetViewport( &saved_vp );

    D3DSURFACE_DESC desc;
    source->GetDesc( &desc );
    if ( ( int ) desc.Width != width || ( int ) desc.Height != height )
        resize( ( int ) desc.Width, ( int ) desc.Height );

    if ( !levels.empty( ) )
    {
        device->StretchRect( source, nullptr, levels[ 0 ].surf, nullptr, D3DTEXF_LINEAR );

        for ( size_t i = 1; i < levels.size( ); i++ )
            device->StretchRect( levels[ i - 1 ].surf, nullptr, levels[ i ].surf, nullptr, D3DTEXF_LINEAR );

        for ( int i = ( int ) levels.size( ) - 2; i >= 0; i-- )
            blend_level( levels[ i ], levels[ i + 1 ].tex );
    }

    if ( saved_rt )
    {
        device->SetRenderTarget( 0, saved_rt );
        saved_rt->Release( );
    }
    if ( saved_ds )
    {
        device->SetDepthStencilSurface( saved_ds );
        saved_ds->Release( );
    }
    device->SetViewport( &saved_vp );
    source->Release( );
}
